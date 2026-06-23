#include "kernel_s.h"
#include <cliente.h>
#include <server.h>
#include <ctype.h>

/* =========================================================
 * Globales del módulo
 * ========================================================= */

t_log    *logger          = NULL;
t_config *config          = NULL;
int       fd_kernel_memory      = -1;   // socket síncrono


/* Dispositivos IO */
int             fd_io_sleep  = -1;
int             fd_io_stdin  = -1;
int             fd_io_stdout = -1;

/* PCBs del planificador */
t_pcb        g_ks_pcbs[KS_MAX_PROCESOS];
pthread_mutex_t g_mutex_ks_pcbs = PTHREAD_MUTEX_INITIALIZER;

t_mutex          g_mutexes[KS_MAX_MUTEX];
pthread_mutex_t  g_mutex_tabla_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int32_t id_cpu;
    int     fd_dispatch;
    int     fd_interrupt;  // -1 si no usa doble canal
    t_pcb  *pcb_actual;
    bool    interrupcion_pendiente;
    uint64_t dispatch_id;
} t_cpu_slot;

t_cpu_slot g_cpus[MAX_CPUS];
pthread_mutex_t g_mutex_cpus = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    t_nodo_ready *head;
    t_nodo_ready *tail;
} t_cola_ready_local;

t_cola_ready_local g_cmn_colas[KS_MAX_PRIORIDADES];
e_algoritmo        g_cmn_algoritmos[KS_MAX_PRIORIDADES];
int32_t            g_cmn_cantidad_colas = 0;

static char *ks_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    return s;
}

static const char *ks_nombre_algoritmo_corto(e_algoritmo algoritmo)
{
    switch (algoritmo) {
        case ALGO_FIFO: return "FIFO";
        case ALGO_RR:   return "RR";
        case ALGO_CMN:  return "CMN";
        default:        return "DESCONOCIDO";
    }
}

static bool ks_parse_algoritmo_cola(const char *token, e_algoritmo *out)
{
    if (!strcmp(token, "FIFO")) {
        *out = ALGO_FIFO;
        return true;
    }

    if (!strcmp(token, "RR")) {
        *out = ALGO_RR;
        return true;
    }

    return false;
}

static void ks_inicializar_colas_cmn(void)
{
    g_cmn_cantidad_colas = 0;
    for (int i = 0; i < KS_MAX_PRIORIDADES; i++) {
        g_cmn_colas[i].head = NULL;
        g_cmn_colas[i].tail = NULL;
        g_cmn_algoritmos[i] = ALGO_FIFO;
    }
}

static bool ks_configurar_colas_cmn(const char *config_colas)
{
    ks_inicializar_colas_cmn();

    if (!config_colas || !*config_colas) {
        log_error(logger, "QUEUES_ALGORITHMS vacio o ausente");
        return false;
    }

    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", config_colas);

    char *inicio = strchr(buffer, '[');
    char *fin = strrchr(buffer, ']');
    if (inicio && fin && fin > inicio) {
        *fin = '\0';
        inicio++;
    } else {
        inicio = buffer;
    }

    char *saveptr = NULL;
    char *token = strtok_r(inicio, ",", &saveptr);
    while (token) {
        if (g_cmn_cantidad_colas >= KS_MAX_PRIORIDADES) {
            log_error(logger, "QUEUES_ALGORITHMS supera el maximo de %d colas", KS_MAX_PRIORIDADES);
            return false;
        }

        char *nombre = ks_trim(token);
        e_algoritmo algoritmo;
        if (!ks_parse_algoritmo_cola(nombre, &algoritmo)) {
            log_error(logger, "Algoritmo de cola CMN invalido: '%s'", nombre);
            return false;
        }

        g_cmn_algoritmos[g_cmn_cantidad_colas] = algoritmo;
        log_info(logger, "CMN: cola prioridad %d configurada con %s",
                 g_cmn_cantidad_colas, ks_nombre_algoritmo_corto(algoritmo));
        g_cmn_cantidad_colas++;

        token = strtok_r(NULL, ",", &saveptr);
    }

    if (g_cmn_cantidad_colas <= 0) {
        log_error(logger, "QUEUES_ALGORITHMS no define ninguna cola");
        return false;
    }

    return true;
}

static bool ks_prioridad_valida_cmn(int32_t prioridad)
{
    return prioridad >= 0 && prioridad < g_cmn_cantidad_colas;
}

static e_algoritmo ks_algoritmo_efectivo_pcb(t_pcb *pcb)
{
    if (g_algoritmo != ALGO_CMN)
        return g_algoritmo;

    if (!pcb || !ks_prioridad_valida_cmn(pcb->prioridad))
        return ALGO_FIFO;

    return g_cmn_algoritmos[pcb->prioridad];
}

static void ks_evaluar_desalojo_cmn(t_pcb *pcb_nuevo)
{
    if (g_algoritmo != ALGO_CMN || !g_preemption || !pcb_nuevo)
        return;

    if (!ks_prioridad_valida_cmn(pcb_nuevo->prioridad))
        return;

    int fd_irq = -1;
    int32_t id_cpu = -1;
    int32_t pid_desalojado = -1;
    int32_t prioridad_desalojado = -1;
    int mejor_slot = -1;

    pthread_mutex_lock(&g_mutex_cpus);
    for (int i = 0; i < MAX_CPUS; i++) {
        t_pcb *actual = g_cpus[i].pcb_actual;
        if (!actual || g_cpus[i].fd_interrupt == -1 || g_cpus[i].interrupcion_pendiente)
            continue;

        if (actual->estado != ESTADO_EXEC)
            continue;

        if (actual->prioridad <= pcb_nuevo->prioridad)
            continue;

        if (mejor_slot == -1 ||
            actual->prioridad > g_cpus[mejor_slot].pcb_actual->prioridad) {
            mejor_slot = i;
        }
    }

    if (mejor_slot != -1) {
        t_pcb *victima = g_cpus[mejor_slot].pcb_actual;
        g_cpus[mejor_slot].interrupcion_pendiente = true;
        fd_irq = g_cpus[mejor_slot].fd_interrupt;
        id_cpu = g_cpus[mejor_slot].id_cpu;
        pid_desalojado = victima->pid;
        prioridad_desalojado = victima->prioridad;
    }
    pthread_mutex_unlock(&g_mutex_cpus);

    if (fd_irq != -1) {
        int32_t motivo = (int32_t)MOTIVO_INTERRUPCION;
        send(fd_irq, &motivo, sizeof(int32_t), MSG_NOSIGNAL);
        log_info(logger,
                 "## (%d) Prioridad: %d - Desalojado por cola mas prioritaria por el proceso %d con prioridad %d",
                 pid_desalojado, prioridad_desalojado,
                 pcb_nuevo->pid, pcb_nuevo->prioridad);
        log_info(logger,
                 "## CPU %d - Interrupcion CMN enviada por prioridad (fd_irq=%d)",
                 id_cpu, fd_irq);
    }
}

/* Semáforo: hay proceso READY para despachar a una CPU */
sem_t g_sem_ready;




void ks_init_mutexes(void)
{
    memset(g_mutexes, 0, sizeof(g_mutexes));
    for (int i = 0; i < KS_MAX_MUTEX; i++) {
        g_mutexes[i].activo   = false;
        g_mutexes[i].tomado   = false;
        g_mutexes[i].pid_duenio = -1;
        g_mutexes[i].cola_espera = NULL;
    }
}

t_mutex *ks_buscar_mutex(const char *nombre)
{
    for (int i = 0; i < KS_MAX_MUTEX; i++)
        if (g_mutexes[i].activo && !strcmp(g_mutexes[i].nombre, nombre))
            return &g_mutexes[i];
    return NULL;
}

void ks_syscall_mutex_create(t_pcb *pcb, const char *nombre)
{
    pthread_mutex_lock(&g_mutex_tabla_mutex);

    // Si ya existe, no hacer nada
    if (ks_buscar_mutex(nombre)) {
        log_info(logger, "## (%d) - MUTEX_CREATE: '%s' ya existe", pcb->pid, nombre);
        pthread_mutex_unlock(&g_mutex_tabla_mutex);
        ks_encolar_ready(pcb);
        return;
    }

    // Buscar slot libre
    t_mutex *m = NULL;
    for (int i = 0; i < KS_MAX_MUTEX; i++) {
        if (!g_mutexes[i].activo) { m = &g_mutexes[i]; break; }
    }

    if (!m) {
        log_error(logger, "## (%d) - MUTEX_CREATE: tabla llena", pcb->pid);
        pthread_mutex_unlock(&g_mutex_tabla_mutex);
        ks_encolar_ready(pcb);
        return;
    }

    strncpy(m->nombre, nombre, sizeof(m->nombre) - 1);
    m->activo    = true;
    m->tomado    = false;
    m->pid_duenio = -1;
    m->cola_espera = NULL;

    log_info(logger, "## (%d) - MUTEX_CREATE: '%s' creado", pcb->pid, nombre);

    pthread_mutex_unlock(&g_mutex_tabla_mutex);
    ks_encolar_ready(pcb);
}

void ks_syscall_mutex_lock(t_pcb *pcb, const char *nombre)
{
    pthread_mutex_lock(&g_mutex_tabla_mutex);

    t_mutex *m = ks_buscar_mutex(nombre);
    if (!m) {
        log_error(logger, "## (%d) - MUTEX_LOCK: '%s' no existe", pcb->pid, nombre);
        pthread_mutex_unlock(&g_mutex_tabla_mutex);
        ks_encolar_ready(pcb);
        return;
    }

    if (!m->tomado) {
        // Mutex libre — tomarlo
        m->tomado    = true;
        m->pid_duenio = pcb->pid;
        m->prioridad_original_duenio = pcb->prioridad;

        log_info(logger,
                 "## (%d) - MUTEX_LOCK: '%s' tomado (estaba libre)",
                 pcb->pid, nombre);

        pthread_mutex_unlock(&g_mutex_tabla_mutex);
        ks_encolar_ready(pcb);

    } else {
        // Mutex ocupado — bloquear proceso
        log_info(logger,
                 "## (%d) - MUTEX_LOCK: '%s' ocupado por PID %d — bloqueando",
                 pcb->pid, nombre, m->pid_duenio);

        // Encolar en la cola de espera del mutex
        t_proceso_esperando *nodo = malloc(sizeof(t_proceso_esperando));
        nodo->pcb       = pcb;
        nodo->siguiente = NULL;

        // Agregar al final de la cola de espera
        if (!m->cola_espera) {
            m->cola_espera = nodo;
        } else {
            t_proceso_esperando *cur = m->cola_espera;
            while (cur->siguiente) cur = cur->siguiente;
            cur->siguiente = nodo;
        }

        // Herencia de prioridades (solo aplica en CMN)
        if (g_algoritmo == ALGO_CMN) {
            t_pcb *duenio = ks_buscar_pcb(m->pid_duenio);
            if (duenio && pcb->prioridad < duenio->prioridad) {
                log_info(logger,
                         "## (%d) - MUTEX_LOCK: herencia de prioridad — "
                         "PID %d hereda prioridad %d (tenía %d)",
                         pcb->pid,
                         duenio->pid,
                         pcb->prioridad,
                         duenio->prioridad);
                duenio->prioridad = pcb->prioridad;
            }
        }

        // Cambiar estado a BLOCK sin encolar en READY
        ks_cambiar_estado(pcb, ESTADO_BLOCK);

        pthread_mutex_unlock(&g_mutex_tabla_mutex);
        // NO llamar a ks_encolar_ready — queda bloqueado
    }
}

void ks_syscall_mutex_unlock(t_pcb *pcb, const char *nombre)
{
    pthread_mutex_lock(&g_mutex_tabla_mutex);

    t_mutex *m = ks_buscar_mutex(nombre);
    if (!m) {
        log_error(logger, "## (%d) - MUTEX_UNLOCK: '%s' no existe", pcb->pid, nombre);
        pthread_mutex_unlock(&g_mutex_tabla_mutex);
        ks_encolar_ready(pcb);
        return;
    }

    if (!m->tomado || m->pid_duenio != pcb->pid) {
        log_warning(logger,
                    "## (%d) - MUTEX_UNLOCK: '%s' no está tomado por este proceso",
                    pcb->pid, nombre);
        pthread_mutex_unlock(&g_mutex_tabla_mutex);
        ks_encolar_ready(pcb);
        return;
    }

    // Restaurar prioridad original si hubo herencia
    if (g_algoritmo == ALGO_CMN &&
        pcb->prioridad != m->prioridad_original_duenio) {
        log_info(logger,
                 "## (%d) - MUTEX_UNLOCK: restaurando prioridad original %d (tenía %d por herencia)",
                 pcb->pid, m->prioridad_original_duenio, pcb->prioridad);
        pcb->prioridad = m->prioridad_original_duenio;
    }

    // ¿Hay procesos esperando?
    if (m->cola_espera) {
        // Dar el mutex al primero de la cola
        t_proceso_esperando *nodo = m->cola_espera;
        m->cola_espera            = nodo->siguiente;

        t_pcb *siguiente = nodo->pcb;
        free(nodo);

        m->pid_duenio = siguiente->pid;
        m->prioridad_original_duenio = siguiente->prioridad;
        // m->tomado sigue en true — lo toma el siguiente

        log_info(logger,
                 "## (%d) - MUTEX_UNLOCK: '%s' cedido a PID %d",
                 pcb->pid, nombre, siguiente->pid);

        pthread_mutex_unlock(&g_mutex_tabla_mutex);

        // Desbloquear al siguiente
        ks_encolar_ready(siguiente);

    } else {
        // Nadie esperaba — liberar
        m->tomado     = false;
        m->pid_duenio = -1;

        log_info(logger,
                 "## (%d) - MUTEX_UNLOCK: '%s' liberado (nadie esperaba)",
                 pcb->pid, nombre);

        pthread_mutex_unlock(&g_mutex_tabla_mutex);
    }

    // El proceso que hizo unlock vuelve a READY
    ks_encolar_ready(pcb);
}


/* =========================================================
 * Helpers de estado
 * ========================================================= */

const char *ks_nombre_estado(e_estado_proceso e)
{
    switch (e) {
        case ESTADO_NEW:        return "NEW";
        case ESTADO_READY:      return "READY";
        case ESTADO_EXEC:       return "EXEC";
        case ESTADO_BLOCK:      return "BLOCK";
        case ESTADO_SUSP_READY: return "SUSP_READY";
        case ESTADO_SUSP_BLOCK: return "SUSP_BLOCK";
        case ESTADO_EXIT:       return "EXIT";
        default:                return "DESCONOCIDO";
    }
}

void ks_cambiar_estado(t_pcb *pcb, e_estado_proceso nuevo)
{
    log_info(logger, "## (%d) Pasa del estado %s al estado %s",
             pcb->pid,
             ks_nombre_estado(pcb->estado),
             ks_nombre_estado(nuevo));
    pcb->estado = nuevo;
}

/* =========================================================
 * Gestión de PCBs del planificador
 * ========================================================= */

t_pcb *ks_crear_pcb(int32_t pid, int32_t prioridad)
{
    pthread_mutex_lock(&g_mutex_ks_pcbs);
    t_pcb *pcb = NULL;
    for (int i = 0; i < KS_MAX_PROCESOS; i++) {
        if (!g_ks_pcbs[i].activo) {
            memset(&g_ks_pcbs[i], 0, sizeof(t_pcb));
            g_ks_pcbs[i].pid       = pid;
            g_ks_pcbs[i].prioridad = prioridad;
            g_ks_pcbs[i].estado    = ESTADO_NEW;
            g_ks_pcbs[i].activo    = true;
            pcb = &g_ks_pcbs[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex_ks_pcbs);
    return pcb;
}

t_pcb *ks_buscar_pcb(int32_t pid)
{
    pthread_mutex_lock(&g_mutex_ks_pcbs);
    t_pcb *pcb = NULL;
    for (int i = 0; i < KS_MAX_PROCESOS; i++)
        if (g_ks_pcbs[i].activo && g_ks_pcbs[i].pid == pid)
        { pcb = &g_ks_pcbs[i]; break; }
    pthread_mutex_unlock(&g_mutex_ks_pcbs);
    return pcb;
}

/* Busca el primer proceso en estado READY (FIFO simple) */
t_pcb *ks_primer_ready(void)
{
    pthread_mutex_lock(&g_mutex_ks_pcbs);
    t_pcb *pcb = NULL;
    for (int i = 0; i < KS_MAX_PROCESOS; i++)
        if (g_ks_pcbs[i].activo && g_ks_pcbs[i].estado == ESTADO_READY)
        { pcb = &g_ks_pcbs[i]; break; }
    pthread_mutex_unlock(&g_mutex_ks_pcbs);
    return pcb;
}

/* =========================================================
 * Registro de dispositivos IO
 * ========================================================= */

void ks_registrar_io(int fd, int32_t subtipo)
{
    pthread_mutex_lock(&mutex_io);
    switch (subtipo) {
        case OP_IO_SLEEP:
            if (fd_io_sleep != -1) close(fd_io_sleep);
            fd_io_sleep = fd;
            log_info(logger, "## Dispositivo IO SLEEP conectado (fd=%d)", fd);
            break;
        case OP_IO_STDIN:
            if (fd_io_stdin != -1) close(fd_io_stdin);
            fd_io_stdin = fd;
            log_info(logger, "## Dispositivo IO STDIN conectado (fd=%d)", fd);
            break;
        case OP_IO_STDOUT:
            if (fd_io_stdout != -1) close(fd_io_stdout);
            fd_io_stdout = fd;
            log_info(logger, "## Dispositivo IO STDOUT conectado (fd=%d)", fd);
            break;
        default:
            log_warning(logger, "Subtipo IO desconocido: %d (fd=%d)", subtipo, fd);
            close(fd);
    }
    pthread_mutex_unlock(&mutex_io);
}

int ks_fd_io(int32_t subtipo)
{
    switch (subtipo) {
        case OP_IO_SLEEP:  return fd_io_sleep;
        case OP_IO_STDIN:  return fd_io_stdin;
        case OP_IO_STDOUT: return fd_io_stdout;
        default:                return -1;
    }
}

const char *ks_nombre_io(int32_t subtipo)
{
    switch (subtipo) {
        case OP_IO_SLEEP:  return "SLEEP";
        case OP_IO_STDIN:  return "STDIN";
        case OP_IO_STDOUT: return "STDOUT";
        default:                return "DESCONOCIDO";
    }
}

typedef struct s_io_request {
    int32_t pid;
    int32_t subtipo;
    int32_t param_size;
    char   *param;
    uint32_t dir_logica;
    int32_t  tamanio;
    struct s_io_request *siguiente;
} t_io_request;

t_io_request *g_io_queue_head = NULL;
t_io_request *g_io_queue_tail = NULL;
pthread_mutex_t g_mutex_io_queue = PTHREAD_MUTEX_INITIALIZER;
sem_t g_sem_io_request;

static t_io_request *ks_io_request_crear(int32_t pid, int32_t subtipo,
                                         const void *param, int32_t param_size,
                                         uint32_t dir_logica, int32_t tamanio)
{
    t_io_request *req = malloc(sizeof(t_io_request));
    if (!req) return NULL;

    req->pid = pid;
    req->subtipo = subtipo;
    req->dir_logica = dir_logica;
    req->tamanio = tamanio;
    req->siguiente = NULL;

    if (param_size > 0) {
        req->param = malloc(param_size);
        if (!req->param) { free(req); return NULL; }
        memcpy(req->param, param, param_size);
    } else {
        req->param = NULL;
    }
    req->param_size = param_size;

    return req;
}

static void ks_io_request_destruir(t_io_request *req)
{
    if (!req) return;
    free(req->param);
    free(req);
}

static void ks_io_request_enqueuar(t_io_request *req)
{
    pthread_mutex_lock(&g_mutex_io_queue);
    if (g_io_queue_tail) {
        g_io_queue_tail->siguiente = req;
    } else {
        g_io_queue_head = req;
    }
    g_io_queue_tail = req;
    pthread_mutex_unlock(&g_mutex_io_queue);
    sem_post(&g_sem_io_request);
}

static t_io_request *ks_io_request_dequeuar(void)
{
    sem_wait(&g_sem_io_request);
    pthread_mutex_lock(&g_mutex_io_queue);
    t_io_request *req = g_io_queue_head;
    if (req) {
        g_io_queue_head = req->siguiente;
        if (!g_io_queue_head) {
            g_io_queue_tail = NULL;
        }
    }
    pthread_mutex_unlock(&g_mutex_io_queue);
    return req;
}

void ks_io_request_completo(t_io_request *req, void *datos, int32_t datos_size)
{
    t_pcb *pcb = ks_buscar_pcb(req->pid);
    if (!pcb) {
        log_warning(logger, "IO completado para PID %d pero PCB no existe", req->pid);
        return;
    }

    if (req->subtipo == OP_IO_STDIN) {
        log_info(logger, "## (%d) - STDIN completado: %d bytes recibidos, escribiendo en KM...", req->pid, datos_size);

        if (!ks_escribir_datos(req->pid, req->dir_logica, req->tamanio, datos)) {
            log_error(logger, "## (%d) - STDIN: fallo escritura en memoria (SEG_FAULT)", req->pid);
            ks_cambiar_estado(pcb, ESTADO_EXIT);
            return;
        }

        log_info(logger, "## (%d) - STDIN: datos escritos correctamente en memoria", req->pid);
    } else {
        log_info(logger, "## (%d) - IO %s completado", req->pid, ks_nombre_io(req->subtipo));
    }

    ks_encolar_ready(pcb);
}

void *ks_io_worker(void *arg)
{
    (void)arg;

    while (1) {
        t_io_request *req = ks_io_request_dequeuar();
        if (!req) continue;

        int fd = ks_fd_io(req->subtipo);
        if (fd == -1) {
            log_error(logger,
                      "## IO %s no disponible para PID %d",
                      ks_nombre_io(req->subtipo), req->pid);
            ks_io_request_destruir(req);
            continue;
        }

        enviar_int32(fd, req->subtipo);
        enviar_int32(fd, req->pid);

        if (req->subtipo == OP_IO_SLEEP || req->subtipo == OP_IO_STDIN) {
            int32_t valor;
            memcpy(&valor, req->param, sizeof(int32_t));
            enviar_int32(fd, valor);
        } else if (req->subtipo == OP_IO_STDOUT) {
            int32_t tamanio;
            memcpy(&tamanio, req->param, sizeof(int32_t));
            enviar_int32(fd, tamanio);
            send(fd, req->param + sizeof(int32_t), tamanio, MSG_NOSIGNAL);
        }

        int32_t resp;
        if (!recibir_int32(fd, &resp) || resp != OP_OK) {
            log_error(logger,
                      "## IO %s fallo para PID %d", ks_nombre_io(req->subtipo), req->pid);
            ks_io_request_destruir(req);
            continue;
        }

        if (req->subtipo == OP_IO_STDIN) {
            int32_t tam;
            if (!recibir_int32(fd, &tam)) {
                log_error(logger,
                          "## STDIN — error recibiendo tamanio para PID %d", req->pid);
                ks_io_request_destruir(req);
                continue;
            }

            void *datos = malloc(tam);
            if (!datos) {
                log_error(logger,
                          "## STDIN — malloc fallo para PID %d", req->pid);
                ks_io_request_destruir(req);
                continue;
            }

            if (recv(fd, datos, tam, MSG_WAITALL) != tam) {
                log_error(logger,
                          "## STDIN — error recibiendo datos para PID %d", req->pid);
                free(datos);
                ks_io_request_destruir(req);
                continue;
            }

            ks_io_request_completo(req, datos, tam);
            free(datos);
        } else {
            ks_io_request_completo(req, NULL, 0);
        }

        ks_io_request_destruir(req);
    }
    return NULL;
}


/* =========================================================
 * ks_recibir_de_km
 * Lee un int32 del socket KM descartando ciertos opcodes
 * y procesando eventos de compactación.
 * DEBE llamarse con mutex_km_socket ya tomado.
 * ========================================================= */
bool ks_recibir_de_km(int32_t *out)
{
    while (recibir_int32(fd_kernel_memory, out)) {
        if (*out == OP_NUEVA_MEMORIA) {
            int32_t nueva_total;
            recibir_int32(fd_kernel_memory, &nueva_total);
            log_info(logger,
                     "## KM — OP_NUEVA_MEMORIA (notif. inline): total=%d bytes",
                     nueva_total);
            continue;
        }
        if (*out == OP_MEMORIA_CORRUPTA) {
            log_error(logger, "## KM — OP_MEMORIA_CORRUPTA — BSOD");
            pthread_mutex_lock(&g_mutex_ks_pcbs);
            for (int i = 0; i < KS_MAX_PROCESOS; i++)
                if (g_ks_pcbs[i].activo && g_ks_pcbs[i].estado != ESTADO_EXIT)
                    g_ks_pcbs[i].estado = ESTADO_EXIT;
            pthread_mutex_unlock(&g_mutex_ks_pcbs);
            exit(1);
        }
        if (*out == OP_INICIAR_COMPACT) {
            log_info(logger, "## Inicio de compactación");
            /* Enviar confirmación de desalojo al KM */
            log_info(logger, "Enviando OP_CONFIRMAR_DESALOJO al KM");
            enviar_int32(fd_kernel_memory, OP_CONFIRMAR_DESALOJO);
            continue;  /* seguir esperando la respuesta real */
        }
        if (*out == OP_FIN_COMPACT) {
            log_info(logger, "## Fin de compactación");
            continue;  /* seguir esperando la respuesta real */
        }
        return true;  /* es una respuesta real: OP_OK o OP_ERROR */
    }
    return false;  /* socket cerrado */
}

bool ks_crear_segmento(int32_t pid, int32_t seg_id, int32_t tamanio)
{
    log_info(logger, "## (%d) - MEM_ALLOC: solicitando segmento %d tam=%d al KM",
             pid, seg_id, tamanio);

    pthread_mutex_lock(&mutex_km_socket);

    enviar_int32(fd_kernel_memory, OP_CREAR_SEGMENTO);
    enviar_int32(fd_kernel_memory, pid);
    enviar_int32(fd_kernel_memory, seg_id);
    enviar_int32(fd_kernel_memory, tamanio);

    int32_t resp;
    if (!ks_recibir_de_km(&resp) || resp != OP_OK) {
        log_error(logger, "## (%d) - MEM_ALLOC: KM respondió error", pid);
        pthread_mutex_unlock(&mutex_km_socket);
        return false;
    }

    int32_t n_trozos;
    recibir_int32(fd_kernel_memory, &n_trozos);
    log_info(logger, "## (%d) - MEM_ALLOC: segmento %d creado en %d trozo(s)",
             pid, seg_id, n_trozos);

    for (int i = 0; i < n_trozos; i++) {
        int32_t ms_id, dir_fisica, offset_seg, tam;
        recibir_int32(fd_kernel_memory, &ms_id);
        recibir_int32(fd_kernel_memory, &dir_fisica);
        recibir_int32(fd_kernel_memory, &offset_seg);
        recibir_int32(fd_kernel_memory, &tam);
        log_info(logger, "## (%d) - MEM_ALLOC trozo %d: MS=%d dir_fis=%d offset=%d tam=%d",
                 pid, i, ms_id, dir_fisica, offset_seg, tam);
    }

    pthread_mutex_unlock(&mutex_km_socket);
    return true;
}

bool ks_leer_datos(int32_t pid, int32_t dir_logica, int32_t tamanio, void *buffer)
{
    pthread_mutex_lock(&mutex_km_socket);

    enviar_int32(fd_kernel_memory, OP_LEER_DATOS);
    enviar_int32(fd_kernel_memory, pid);
    enviar_int32(fd_kernel_memory, dir_logica);
    enviar_int32(fd_kernel_memory, tamanio);

    int32_t resp;
    if (!ks_recibir_de_km(&resp) || resp != OP_OK) {
        pthread_mutex_unlock(&mutex_km_socket);
        return false;
    }

    int32_t tam_recv;
    if (!recibir_int32(fd_kernel_memory, &tam_recv) || tam_recv != tamanio) {
        pthread_mutex_unlock(&mutex_km_socket);
        return false;
    }

    if (recv(fd_kernel_memory, buffer, tamanio, MSG_WAITALL) != tamanio) {
        pthread_mutex_unlock(&mutex_km_socket);
        return false;
    }

    pthread_mutex_unlock(&mutex_km_socket);
    return true;
}

bool ks_escribir_datos(int32_t pid, int32_t dir_logica, int32_t tamanio, void *datos)
{
    pthread_mutex_lock(&mutex_km_socket);

    enviar_int32(fd_kernel_memory, OP_ESCRIBIR_DATOS);
    enviar_int32(fd_kernel_memory, pid);
    enviar_int32(fd_kernel_memory, dir_logica);
    enviar_int32(fd_kernel_memory, tamanio);
    send(fd_kernel_memory, datos, tamanio, MSG_NOSIGNAL);

    int32_t resp;
    bool ok = ks_recibir_de_km(&resp) && resp == OP_OK;

    pthread_mutex_unlock(&mutex_km_socket);
    return ok;
}

/* =========================================================
 * Procesamiento de syscall recibida desde la CPU
 * Parsea la instrucción del PCB y despacha al dispositivo correcto.
 * ========================================================= */

void ks_procesar_syscall(t_pcb *pcb)
{
    const char *op  = pcb->syscall_nombre;
    const char *a1  = pcb->syscall_arg1;

    log_info(logger, "## (%d) - Solicitó syscall: %s", pcb->pid, op);

    if (!strcmp(op, "SLEEP")) {
        int32_t ms = atoi(a1);
        log_info(logger, "## (%d) - SLEEP: %d ms", pcb->pid, ms);

        // Cambiar a BLOCK y encolar request de IO
        ks_cambiar_estado(pcb, ESTADO_BLOCK);

        t_io_request *req = ks_io_request_crear(pcb->pid, OP_IO_SLEEP, &ms, sizeof(ms), 0, 0);
        if (!req) {
            log_error(logger, "SLEEP: no se pudo crear request para PID %d", pcb->pid);
            ks_cambiar_estado(pcb, ESTADO_EXIT);
            return;
        }

        ks_io_request_enqueuar(req);
    } else if (!strcmp(op, "STDIN")) {
        uint32_t dir_logica = pcb->syscall_val1;
        int32_t  tamanio    = (int32_t)pcb->syscall_val2;

        log_info(logger, "## (%d) - STDIN: dir_logica=%u, tamanio=%d", pcb->pid, dir_logica, tamanio);

        // Cambiar a BLOCK y encolar request de IO
        ks_cambiar_estado(pcb, ESTADO_BLOCK);

        t_io_request *req = ks_io_request_crear(pcb->pid, OP_IO_STDIN,
                                                &tamanio, sizeof(tamanio),
                                                dir_logica, tamanio);
        if (!req) {
            log_error(logger, "STDIN: no se pudo crear request para PID %d", pcb->pid);
            ks_cambiar_estado(pcb, ESTADO_EXIT);
            return;
        }

        ks_io_request_enqueuar(req);
    
    } else if (!strcmp(op, "STDOUT")) {
        uint32_t dir_logica = pcb->syscall_val1;
        int32_t  tamanio    = (int32_t)pcb->syscall_val2;

        log_info(logger, "## (%d) - STDOUT: dir_logica=%u, tamanio=%d", pcb->pid, dir_logica, tamanio);

        // Leer datos de memoria antes de enviar a IO
        void *buffer = malloc(tamanio);
        if (!buffer) {
            log_error(logger, "STDOUT: malloc falló para PID %d", pcb->pid);
            ks_cambiar_estado(pcb, ESTADO_EXIT);
            return;
        }

        if (!ks_leer_datos(pcb->pid, dir_logica, tamanio, buffer)) {
            log_error(logger, "STDOUT: fallo lectura de memoria para PID %d (SEG_FAULT)", pcb->pid);
            free(buffer);
            ks_cambiar_estado(pcb, ESTADO_EXIT);
            return;
        }

        // Cambiar a BLOCK y encolar request de IO con los datos leídos
        ks_cambiar_estado(pcb, ESTADO_BLOCK);

        // Empaquetar: [int32_t tamanio][datos...]
        int32_t param_size = sizeof(int32_t) + tamanio;
        void *param = malloc(param_size);
        memcpy(param, &tamanio, sizeof(int32_t));
        memcpy((char*)param + sizeof(int32_t), buffer, tamanio);
        free(buffer);

        t_io_request *req = ks_io_request_crear(pcb->pid, OP_IO_STDOUT,
                                            param, param_size,  // ← param con header
                                            0, 0);
        free(param);

        if (!req) {
            log_error(logger, "STDOUT: no se pudo crear request para PID %d", pcb->pid);
            ks_cambiar_estado(pcb, ESTADO_EXIT);
            return;
        }

        ks_io_request_enqueuar(req);
} else if (!strcmp(op, "INIT_PROC")) {

        const char *nombre_script = pcb->syscall_arg1;
        const char *prio_str      = pcb->syscall_arg2;
        int32_t     prioridad     = atoi(prio_str);

        if (g_algoritmo == ALGO_CMN && !ks_prioridad_valida_cmn(prioridad)) {
            log_error(logger,
                      "## (%d) - INIT_PROC: prioridad %d fuera de rango CMN [0,%d], no se crea proceso",
                      pcb->pid, prioridad, g_cmn_cantidad_colas - 1);
            ks_encolar_ready(pcb);
            return;
        }

        static int32_t g_next_pid = 1;
        int32_t nuevo_pid = __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_SEQ_CST);

        /* ── Log de contexto ── */
        log_info(logger,
                "## (%d) - INIT_PROC: script='%s' prioridad=%d → asignando PID %d",
                pcb->pid, nombre_script, prioridad, nuevo_pid);

        log_info(logger,
                "## (%d) - INIT_PROC: algoritmo activo = %s",
                pcb->pid,
                g_algoritmo == ALGO_FIFO ? "FIFO" :
                g_algoritmo == ALGO_RR   ? "RR"   : "CMN");

        if (g_algoritmo == ALGO_FIFO || g_algoritmo == ALGO_RR) {
            log_info(logger,
                    "## (%d) - INIT_PROC: [%s] prioridad del hijo (%d) NO tiene injerencia — "
                    "se usará orden de llegada",
                    pcb->pid,
                    g_algoritmo == ALGO_FIFO ? "FIFO" : "RR",
                    prioridad);
        } else {
            log_info(logger,
                    "## (%d) - INIT_PROC: [CMN] prioridad del hijo (%d) DETERMINA posición en cola — "
                    "QUEUE_PREEMPTION=%s",
                    pcb->pid, prioridad,
                    g_preemption ? "TRUE" : "FALSE");
        }

        /* ── Notificar al KM (con mutex para serializar el socket) ── */
        log_info(logger,
                "## (%d) - INIT_PROC: enviando OP_CREAR_PROCESO al KM "
                "(PID=%d script='%s' prioridad=%d)",
                pcb->pid, nuevo_pid, nombre_script, prioridad);

        pthread_mutex_lock(&mutex_km_socket);

        t_paquete *paq = crear_paquete(OP_CREAR_PROCESO, logger);
        agregar_a_paquete(paq, &nuevo_pid,           sizeof(int32_t));
        agregar_a_paquete(paq, &prioridad,           sizeof(int32_t));
        agregar_a_paquete(paq, (void*)nombre_script, strlen(nombre_script));
        enviar_paquete(paq, fd_kernel_memory, logger);
        eliminar_paquete(paq);

        int32_t resp_km;
        bool km_ok = ks_recibir_de_km(&resp_km) && resp_km == OP_OK; // ← antes: recibir_int32

        pthread_mutex_unlock(&mutex_km_socket);

        if (!km_ok) {
            log_error(logger,
                    "## (%d) - INIT_PROC: KM respondió ERROR — script '%s' no encontrado o inválido",
                    pcb->pid, nombre_script);
            log_info(logger,
                    "## (%d) - INIT_PROC: proceso padre vuelve a READY sin crear hijo",
                    pcb->pid);
            ks_encolar_ready(pcb);
            return;
        }

        log_info(logger,
                "## (%d) - INIT_PROC: KM confirmó creación de PID %d",
                pcb->pid, nuevo_pid);

        /* ── Crear PCB del hijo ── */
        t_pcb *hijo = ks_crear_pcb(nuevo_pid, prioridad);
        if (!hijo) {
            log_error(logger,
                    "## (%d) - INIT_PROC: sin espacio para PCB de PID %d — "
                    "proceso padre vuelve a READY",
                    pcb->pid, nuevo_pid);
            ks_encolar_ready(pcb);
            return;
        }
        log_info(logger, "## (<%d>) Se crea el proceso - Estado: NEW", nuevo_pid);

        /* ── Encolar según algoritmo ── */
        if (g_algoritmo == ALGO_FIFO) {
            log_info(logger,
                    "## (%d) - INIT_PROC: [FIFO] hijo PID %d → FINAL de READY "
                    "(espera que proceso actual finalice con EXIT)",
                    pcb->pid, nuevo_pid);
            ks_encolar_ready(hijo);

            log_info(logger,
                    "## (%d) - INIT_PROC: [FIFO] padre PID %d → FINAL de READY "
                    "(detrás del hijo recién creado)",
                    pcb->pid, pcb->pid);
            ks_encolar_ready(pcb);

        } else if (g_algoritmo == ALGO_RR) {
            log_info(logger,
                    "## (%d) - INIT_PROC: [RR] hijo PID %d → FINAL de READY "
                    "(quantum=%d ms, espera su turno)",
                    pcb->pid, nuevo_pid, g_quantum_ms);
            ks_encolar_ready(hijo);

            log_info(logger,
                    "## (%d) - INIT_PROC: [RR] padre PID %d → FINAL de READY "
                    "(quantum=%d ms, espera su turno)",
                    pcb->pid, pcb->pid, g_quantum_ms);
            ks_encolar_ready(pcb);

        } else {
            /* CMN */
            log_info(logger,
                    "## (%d) - INIT_PROC: [CMN] hijo PID %d prioridad=%d → "
                    "posición en cola por prioridad",
                    pcb->pid, nuevo_pid, prioridad);
            ks_encolar_ready(hijo);

            log_info(logger,
                    "## (%d) - INIT_PROC: [CMN] padre PID %d prioridad=%d → "
                    "posición en cola por prioridad",
                    pcb->pid, pcb->prioridad, pcb->pid);
            ks_encolar_ready(pcb);

            if (g_preemption) {
                log_info(logger,
                        "## (%d) - INIT_PROC: [CMN] QUEUE_PREEMPTION=TRUE — "
                        "desalojo por prioridad evaluado para hijo PID %d (prio=%d)",
                        pcb->pid, nuevo_pid, prioridad);
            }
        }

    } else if (!strcmp(op, "EXIT")) {
        ks_cambiar_estado(pcb, ESTADO_EXIT);
        log_info(logger, "## (%d) finalizó su ejecución con motivo de EXIT", pcb->pid);

    } 
    
    else if (!strcmp(op, "MEM_ALLOC")) {

        int32_t seg_id = atoi(pcb->syscall_arg1);
        int32_t tamanio = atoi(pcb->syscall_arg2);
        
        log_info(logger, "## (%d) - Solicitó syscall: MEM_ALLOC seg=%d tam=%d", pcb->pid, seg_id, tamanio);

        if (ks_crear_segmento(pcb->pid, seg_id, tamanio)) {
            ks_encolar_ready(pcb);
        } else {
            ks_cambiar_estado(pcb, ESTADO_EXIT);
        }
    
    } else if (!strcmp(op, "MUTEX_CREATE")) {
        ks_syscall_mutex_create(pcb, pcb->syscall_arg1);

    } else if (!strcmp(op, "MUTEX_LOCK")) {
        ks_syscall_mutex_lock(pcb, pcb->syscall_arg1);

    } else if (!strcmp(op, "MUTEX_UNLOCK")) {
        ks_syscall_mutex_unlock(pcb, pcb->syscall_arg1);

    } else if (!strcmp(op, "MEM_FREE")) {
        int32_t seg_id = atoi(pcb->syscall_arg1);
        
        log_info(logger, "## (%d) - Solicitó syscall: MEM_FREE seg=%d", pcb->pid, seg_id);
        
        pthread_mutex_lock(&mutex_km_socket);
        enviar_int32(fd_kernel_memory, OP_ELIMINAR_SEGMENTO);
        enviar_int32(fd_kernel_memory, pcb->pid);
        enviar_int32(fd_kernel_memory, seg_id);
        
        int32_t resp;
        bool ok = ks_recibir_de_km(&resp) && resp == OP_OK;
        pthread_mutex_unlock(&mutex_km_socket);
        
        if (ok)
            log_info(logger, "## (%d) - MEM_FREE: segmento %d liberado", pcb->pid, seg_id);
        else
            log_error(logger, "## (%d) - MEM_FREE: error liberando segmento %d", pcb->pid, seg_id);
        
        ks_encolar_ready(pcb);

    } else {
        log_warning(logger, "## (%d) - syscall desconocida: %s", pcb->pid, op);
        ks_encolar_ready(pcb);
    }
}

/* =========================================================
 * atender_cliente_ks
 * Hilo por conexión: identifica tipo (CPU / IO) y actúa.
 * Para la CPU: planificador simplificado FIFO.
 * ========================================================= */

typedef struct {
    t_pcb  *pcb;
    int     fd_cpu;       // fd de dispatch (no se usa para enviar IRQ)
    int     fd_interrupt; // fd del canal de interrupciones (-1 si no hay)
    int32_t id_cpu;
    int32_t quantum_ms;
    uint64_t dispatch_id;
} t_quantum_arg;

void *ks_hilo_quantum(void *varg)
{
    t_quantum_arg *arg = (t_quantum_arg *)varg;

    struct timespec ts = {
        .tv_sec  = arg->quantum_ms / 1000,
        .tv_nsec = (arg->quantum_ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);

    /* Si el proceso sigue en EXEC, enviar interrupción */
    pthread_mutex_lock(&g_mutex_ks_pcbs);
    bool sigue_exec = (arg->pcb->estado == ESTADO_EXEC);
    pthread_mutex_unlock(&g_mutex_ks_pcbs);

    if (sigue_exec) {
        int fd_irq = arg->fd_interrupt; // fd del canal de interrupciones
        bool puede_interrumpir = false;

        if (fd_irq != -1) {
            pthread_mutex_lock(&g_mutex_cpus);
            for (int i = 0; i < MAX_CPUS; i++) {
                if (g_cpus[i].fd_dispatch == arg->fd_cpu &&
                    g_cpus[i].dispatch_id == arg->dispatch_id &&
                    g_cpus[i].pcb_actual == arg->pcb &&
                    !g_cpus[i].interrupcion_pendiente) {
                    g_cpus[i].interrupcion_pendiente = true;
                    puede_interrumpir = true;
                    break;
                }
            }
            pthread_mutex_unlock(&g_mutex_cpus);
        }

        if (fd_irq != -1 && puede_interrumpir) {
            int32_t motivo = (int32_t)MOTIVO_INTERRUPCION;
            send(fd_irq, &motivo, sizeof(int32_t), MSG_NOSIGNAL);
            log_info(logger,
                    "## (Q) Interrupción enviada a CPU %d (fd_irq=%d) — PID %d",
                    arg->id_cpu, fd_irq, arg->pcb->pid);
        }
    }

    free(arg);
    return NULL;
}

void *atender_cliente_ks(void *varg)
{
    t_hilo_arg *arg = (t_hilo_arg *)varg;
    int         fd  = arg->fd_cliente;
    free(arg);

    int32_t tipo = recibir_handshake(logger, fd);
    if (tipo == HANDSHAKE_ERR) { close(fd); return NULL; }

    /* ── CPU — handshake extendido ── */
    if (tipo == TIPO_CPU) {
        int32_t id_cpu;
        if (!recibir_int32(fd, &id_cpu)) { close(fd); return NULL; }

        // Leer canal enviado por la CPU (CANAL_CPU_DISPATCH o CANAL_CPU_INTERRUPT)
        int32_t canal;
        if (!recibir_int32(fd, &canal)) { close(fd); return NULL; }

        pthread_mutex_lock(&g_mutex_cpus);

        if (canal == CANAL_CPU_INTERRUPT) {
            /* Segunda conexión de esta CPU — registrar fd de interrupciones */
            for (int i = 0; i < MAX_CPUS; i++) {
                if (g_cpus[i].id_cpu == id_cpu) {
                    g_cpus[i].fd_interrupt = fd;
                    log_info(logger, "## CPU %d canal INTERRUPT registrado (fd=%d)", id_cpu, fd);
                    break;
                }
            }
            pthread_mutex_unlock(&g_mutex_cpus);
            /* Este hilo no entra al loop planificador */
            return NULL;
        }

        /* CANAL_CPU_DISPATCH — registrar slot principal */
        for (int i = 0; i < MAX_CPUS; i++) {
            if (g_cpus[i].fd_dispatch == -1) {
                g_cpus[i].id_cpu       = id_cpu;
                g_cpus[i].fd_dispatch  = fd;
                g_cpus[i].fd_interrupt = -1;
                g_cpus[i].pcb_actual = NULL;
                g_cpus[i].interrupcion_pendiente = false;
                g_cpus[i].dispatch_id = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_mutex_cpus);

        log_info(logger, "## CPU %d Conectada (fd=%d)", id_cpu, fd);


        /*
         * Loop planificador:
         */
        while (1)
        {
            /* Bloquear hasta que haya proceso READY */
            log_info(logger, "CPU %d esperando proceso READY...", id_cpu);
            sem_wait(&g_sem_ready);

            t_pcb *pcb = cola_ready_desencolar();
            if (!pcb) {
                log_warning(logger, "CPU %d: sem_wait sin proceso en cola — señal espuria", id_cpu);
                continue;
            }

            ks_cambiar_estado(pcb, ESTADO_EXEC);
            int fd_irq_actual = -1;
            uint64_t dispatch_id_actual = 0;
            pthread_mutex_lock(&g_mutex_cpus);
            for (int i = 0; i < MAX_CPUS; i++) {
                if (g_cpus[i].fd_dispatch == fd) {
                    g_cpus[i].pcb_actual = pcb;
                    g_cpus[i].interrupcion_pendiente = false;
                    g_cpus[i].dispatch_id++;
                    dispatch_id_actual = g_cpus[i].dispatch_id;
                    fd_irq_actual = g_cpus[i].fd_interrupt;
                    pcb->fd_cpu_asignada = fd;
                    break;
                }
            }
            pthread_mutex_unlock(&g_mutex_cpus);
            log_info(logger, "## CPU %d — enviando PID %d a ejecutar", id_cpu, pcb->pid);

            /* Enviar PID a la CPU */
            enviar_int32(fd, pcb->pid);


            /* ── Arrancar timer de quantum si es RR ── */
            e_algoritmo algoritmo_exec = ks_algoritmo_efectivo_pcb(pcb);
            if (algoritmo_exec == ALGO_RR) {
                t_quantum_arg *qarg = malloc(sizeof(t_quantum_arg));
                qarg->pcb          = pcb;
                qarg->fd_cpu       = fd;
                qarg->fd_interrupt = fd_irq_actual;
                qarg->id_cpu       = id_cpu;
                qarg->quantum_ms   = g_quantum_ms;
                qarg->dispatch_id  = dispatch_id_actual;

                pthread_t hilo_q;
                pthread_create(&hilo_q, NULL, ks_hilo_quantum, qarg);
                pthread_detach(hilo_q);

                log_info(logger,
                        "## CPU %d — PID %d ejecutando con quantum=%d ms (fd_irq=%d)",
                        id_cpu, pcb->pid, g_quantum_ms, fd_irq_actual);
            }

            /* Esperar devolución */
            int32_t pid_ret, motivo;
            if (!recibir_int32(fd, &pid_ret) || !recibir_int32(fd, &motivo)) {
                log_error(logger, "CPU %d — error recibiendo devolución", id_cpu);
                ks_encolar_ready(pcb); /* reencolar para intentar ejecutar en otra CPU */
                break;
            }

            pthread_mutex_lock(&g_mutex_cpus);
            for (int i = 0; i < MAX_CPUS; i++) {
                if (g_cpus[i].fd_dispatch == fd &&
                    g_cpus[i].dispatch_id == dispatch_id_actual &&
                    g_cpus[i].pcb_actual == pcb) {
                    g_cpus[i].pcb_actual = NULL;
                    g_cpus[i].interrupcion_pendiente = false;
                    pcb->fd_cpu_asignada = -1;
                    break;
                }
            }
            pthread_mutex_unlock(&g_mutex_cpus);

            if (motivo == KS_MOTIVO_SYSCALL) {
                int32_t largo;
                if (!recibir_int32(fd, &largo)) break;
                if (largo > 0 && largo < 64) {
                    if (recv(fd, pcb->syscall_nombre, largo, MSG_WAITALL) != largo) break;
                    pcb->syscall_nombre[largo] = '\0';
                } else {
                    pcb->syscall_nombre[0] = '\0';
                }

                if (!recibir_int32(fd, &largo)) break;
                if (largo > 0 && largo < 64) {
                    if (recv(fd, pcb->syscall_arg1, largo, MSG_WAITALL) != largo) break;
                    pcb->syscall_arg1[largo] = '\0';
                } else {
                    pcb->syscall_arg1[0] = '\0';
                }

                if (!recibir_int32(fd, &largo)) break;
                if (largo > 0 && largo < 64) {
                    if (recv(fd, pcb->syscall_arg2, largo, MSG_WAITALL) != largo) break;
                    pcb->syscall_arg2[largo] = '\0';
                } else {
                    pcb->syscall_arg2[0] = '\0';
                }

                int32_t val1, val2;
                if (!recibir_int32(fd, &val1) || !recibir_int32(fd, &val2)) break;

                pcb->syscall_val1 = (uint32_t)val1;
                pcb->syscall_val2 = (uint32_t)val2;

                log_info(logger, "## CPU %d — syscall recibida: '%s' arg1='%s' arg2='%s' vals=%d,%d",
                        id_cpu, pcb->syscall_nombre, pcb->syscall_arg1, pcb->syscall_arg2, val1, val2);
            }

            const char *nombre_motivo;
            switch (motivo) {
                case MOTIVO_EXIT:          nombre_motivo = "EXIT"; break;
                case MOTIVO_SYSCALL:       nombre_motivo = "SYSCALL"; break;
                case MOTIVO_INTERRUPCION:  nombre_motivo = "INTERRUPCION"; break;
                case MOTIVO_ERROR:         nombre_motivo = "ERROR"; break;
                case MOTIVO_SEG_FAULT:     nombre_motivo = "SEG_FAULT"; break;
                case MOTIVO_NINGUNO:       nombre_motivo = "NINGUNO"; break;
                default:                   nombre_motivo = "DESCONOCIDO"; break;
            }
            
            log_info(logger, "## CPU %d devolvio PID %d — motivo: %d (%s)",
                     id_cpu, pid_ret, motivo, nombre_motivo);

            switch (motivo) {
                case KS_MOTIVO_EXIT:
                    ks_cambiar_estado(pcb, ESTADO_EXIT);
                    log_info(logger, "## (%d) finalizó su ejecución con motivo de EXIT",
                             pid_ret);
                    break;

                case KS_MOTIVO_SYSCALL:
                    /*
                     * La instrucción que disparó la syscall está guardada
                     * en el PCB por la CPU antes de devolver el proceso.
                     * El KS la lee y despacha al dispositivo correspondiente.
                     */

                    ks_procesar_syscall(pcb);
                    break;

                case KS_MOTIVO_SEG_FAULT:
                    ks_cambiar_estado(pcb, ESTADO_EXIT);
                    log_info(logger, "## (%d) finalizó su ejecución con motivo de SEG_FAULT",
                             pid_ret);
                    break;

                case KS_MOTIVO_INTERRUPCION:
                    ks_encolar_ready(pcb);
                    break;


                case KS_MOTIVO_NINGUNO:   /* ciclo normal, ej: NOOP */
                    log_info(logger, "## (%d) - CPU completó ciclo sin evento (NOOP u op normal)",
                            pid_ret);
                    ks_encolar_ready(pcb);
                    break;

                default:
                    log_warning(logger, "Motivo desconocido: %d", motivo);
                    ks_encolar_ready(pcb);
                    break;
            }
        }

        log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd);

        /* ── limpiar slot de CPU ── */
        pthread_mutex_lock(&g_mutex_cpus);
        for (int i = 0; i < MAX_CPUS; i++) {
            if (g_cpus[i].fd_dispatch == fd) {
                g_cpus[i].fd_dispatch  = -1;
                g_cpus[i].fd_interrupt = -1;
                g_cpus[i].id_cpu       = -1;
                g_cpus[i].pcb_actual = NULL;
                g_cpus[i].interrupcion_pendiente = false;
                g_cpus[i].dispatch_id = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_mutex_cpus);

        close(fd);
        return NULL;
    }

    /* ── IO ── */
    if (tipo == TIPO_IO) {
        int32_t subtipo;
        if (!recibir_int32(fd, &subtipo)) {
            log_error(logger, "Error recibiendo subtipo IO (fd=%d)", fd);
            close(fd); return NULL;
        }
        ks_registrar_io(fd, subtipo);
        return NULL;
    }

    log_warning(logger, "Tipo desconocido: %d (fd=%d)", tipo, fd);
    close(fd);
    return NULL;
}



/* =========================================================
 * Accept loop para CPUs e IOs
 * ========================================================= */

void iniciar_servidor_puerto_escucha(void)
{
    int puerto = config_get_int_value(config, "PUERTO_ESCUCHA");
    int fd_escucha = iniciar_servidor(logger, puerto);
    if (fd_escucha == -1) {
        log_error(logger, "No se pudo iniciar servidor KS en puerto %d", puerto);
        return;
    }
    log_info(logger, "Kernel Scheduler esperando CPUs/IOs en puerto %d (fd=%d)",
             puerto, fd_escucha);

    while (1) {
        int fd_cliente = esperar_cliente(logger, fd_escucha);
        if (fd_cliente == -1) continue;

        t_hilo_arg *arg = malloc(sizeof(t_hilo_arg));
        if (!arg) { close(fd_cliente); continue; }
        arg->logger     = logger;
        arg->fd_cliente = fd_cliente;

        pthread_t hilo;
        pthread_create(&hilo, NULL, atender_cliente_ks, arg);
        pthread_detach(hilo);
    }
    close(fd_escucha);
}


/* Encola al FINAL — usado por FIFO, RR y padre en CMN */
void cola_ready_encolar_final(t_pcb *pcb)
{
    t_nodo_ready *nodo = malloc(sizeof(t_nodo_ready));
    nodo->pcb       = pcb;
    nodo->siguiente = NULL;

    pthread_mutex_lock(&g_mutex_cola_ready);
    if (!g_cola_ready_tail) {
        g_cola_ready_head = g_cola_ready_tail = nodo;
    } else {
        g_cola_ready_tail->siguiente = nodo;
        g_cola_ready_tail = nodo;
    }
    pthread_mutex_unlock(&g_mutex_cola_ready);
}

/* Encola al FRENTE — usado cuando un proceso desalojado vuelve al inicio */
void cola_ready_encolar_frente(t_pcb *pcb)
{
    t_nodo_ready *nodo = malloc(sizeof(t_nodo_ready));
    nodo->pcb       = pcb;
    nodo->siguiente = NULL;

    pthread_mutex_lock(&g_mutex_cola_ready);
    nodo->siguiente   = g_cola_ready_head;
    g_cola_ready_head = nodo;
    if (!g_cola_ready_tail)
        g_cola_ready_tail = nodo;
    pthread_mutex_unlock(&g_mutex_cola_ready);
}

/* Encola por prioridad — usado por CMN
 * Prioridad 0 = máxima. Igual prioridad → al final del grupo (FIFO dentro del nivel) */
void cola_ready_encolar_cmn(t_pcb *pcb)
{
    if (!ks_prioridad_valida_cmn(pcb->prioridad)) {
        log_error(logger,
                  "## (%d) - CMN: prioridad %d fuera de rango [0,%d], no se encola",
                  pcb->pid, pcb->prioridad, g_cmn_cantidad_colas - 1);
        ks_cambiar_estado(pcb, ESTADO_EXIT);
        return;
    }

    t_nodo_ready *nuevo_cmn = malloc(sizeof(t_nodo_ready));
    if (!nuevo_cmn) {
        log_error(logger, "CMN: malloc fallo al encolar PID %d", pcb->pid);
        ks_cambiar_estado(pcb, ESTADO_EXIT);
        return;
    }
    nuevo_cmn->pcb = pcb;
    nuevo_cmn->siguiente = NULL;

    pthread_mutex_lock(&g_mutex_cola_ready);
    t_cola_ready_local *cola = &g_cmn_colas[pcb->prioridad];
    if (!cola->tail) {
        cola->head = cola->tail = nuevo_cmn;
    } else {
        cola->tail->siguiente = nuevo_cmn;
        cola->tail = nuevo_cmn;
    }
    pthread_mutex_unlock(&g_mutex_cola_ready);
    return;

    t_nodo_ready *nuevo = malloc(sizeof(t_nodo_ready));
    nuevo->pcb       = pcb;
    nuevo->siguiente = NULL;

    pthread_mutex_lock(&g_mutex_cola_ready);

    if (!g_cola_ready_head) {
        g_cola_ready_head = g_cola_ready_tail = nuevo;
    } else if (pcb->prioridad < g_cola_ready_head->pcb->prioridad) {
        /* Mayor prioridad que todos — va al frente */
        nuevo->siguiente  = g_cola_ready_head;
        g_cola_ready_head = nuevo;
    } else {
        /* Buscar posición: insertar DESPUÉS de los nodos con prioridad <= pcb->prioridad
         * (FIFO dentro del mismo nivel de prioridad) */
        t_nodo_ready *cur = g_cola_ready_head;
        while (cur->siguiente &&
               cur->siguiente->pcb->prioridad <= pcb->prioridad)
            cur = cur->siguiente;
        nuevo->siguiente = cur->siguiente;
        cur->siguiente   = nuevo;
        if (!nuevo->siguiente)
            g_cola_ready_tail = nuevo;
    }

    pthread_mutex_unlock(&g_mutex_cola_ready);
}

/* Desencola el primero */
t_pcb *cola_ready_desencolar(void)
{
    pthread_mutex_lock(&g_mutex_cola_ready);

    if (g_algoritmo == ALGO_CMN) {
        for (int prio = 0; prio < g_cmn_cantidad_colas; prio++) {
            t_cola_ready_local *cola = &g_cmn_colas[prio];
            if (!cola->head)
                continue;

            t_nodo_ready *nodo = cola->head;
            cola->head = nodo->siguiente;
            if (!cola->head)
                cola->tail = NULL;
            pthread_mutex_unlock(&g_mutex_cola_ready);

            t_pcb *pcb = nodo->pcb;
            free(nodo);
            return pcb;
        }

        pthread_mutex_unlock(&g_mutex_cola_ready);
        return NULL;
    }

    if (!g_cola_ready_head) {
        pthread_mutex_unlock(&g_mutex_cola_ready);
        return NULL;
    }
    t_nodo_ready *nodo = g_cola_ready_head;
    g_cola_ready_head  = nodo->siguiente;
    if (!g_cola_ready_head)
        g_cola_ready_tail = NULL;
    pthread_mutex_unlock(&g_mutex_cola_ready);

    t_pcb *pcb = nodo->pcb;
    free(nodo);
    return pcb;
}

/* Encola un PCB en READY según el algoritmo activo y loguea el motivo */
void ks_encolar_ready(t_pcb *pcb)
{
    ks_cambiar_estado(pcb, ESTADO_READY);

    switch (g_algoritmo) {

        case ALGO_FIFO:
            log_info(logger,
                     "## (%d) — [FIFO] prioridad ignorada — encolando al final de READY",
                     pcb->pid);
            cola_ready_encolar_final(pcb);
            break;

        case ALGO_RR:
            log_info(logger,
                     "## (%d) — [RR] prioridad ignorada — encolando al final de READY "
                     "(quantum=%d ms)",
                     pcb->pid, g_quantum_ms);
            cola_ready_encolar_final(pcb);
            break;

        case ALGO_CMN:
            if (!ks_prioridad_valida_cmn(pcb->prioridad)) {
                log_error(logger,
                          "## (%d) - CMN: prioridad %d fuera de rango [0,%d], proceso a EXIT",
                          pcb->pid, pcb->prioridad, g_cmn_cantidad_colas - 1);
                ks_cambiar_estado(pcb, ESTADO_EXIT);
                return;
            }

            log_info(logger,
                     "## (%d) — [CMN] encolando por prioridad %d "
                     "(QUEUE_PREEMPTION=%s)",
                     pcb->pid, pcb->prioridad,
                     g_preemption ? "TRUE" : "FALSE");
            cola_ready_encolar_cmn(pcb);
            break;
    }

    sem_post(&g_sem_ready);

    if (g_algoritmo == ALGO_CMN)
        ks_evaluar_desalojo_cmn(pcb);
}



/* =========================================================
 * main
 * ========================================================= */

 int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s kernel_s.config [nombre_script]\n", argv[0]);
        return 1;
    }

    /* Inicializar PCBs */
    memset(g_ks_pcbs, 0, sizeof(g_ks_pcbs));

    /* ── Inicializar slots de CPU ── */
    for (int i = 0; i < MAX_CPUS; i++) {
        g_cpus[i].fd_dispatch  = -1;
        g_cpus[i].fd_interrupt = -1;
        g_cpus[i].id_cpu       = -1;
        g_cpus[i].pcb_actual = NULL;
        g_cpus[i].interrupcion_pendiente = false;
        g_cpus[i].dispatch_id = 0;
    }

    ks_init_mutexes();

    logger = log_create("kernel_scheduler.log", "KERNEL_SCHEDULER", true, LOG_LEVEL_INFO);
    if (!logger) { fprintf(stderr, "Error creando logger\n"); return 1; }

    config = config_create(argv[1]);
    if (!config) {
        char alt[512];
        snprintf(alt, sizeof(alt), "../%s", argv[1]);
        config = config_create(alt);
    }
    if (!config) {
        log_error(logger, "No se pudo cargar '%s'", argv[1]);
        log_destroy(logger); return 1;
    }

    /* ===== LECTURA DE PARÁMETROS DE CONFIGURACIÓN ===== */
    char *algoritmo_planificacion = config_get_string_value(config, "PLANIFICATION_ALGORITHM");
    char *cola_algoritmos         = config_get_string_value(config, "QUEUES_ALGORITHMS");
    int   quantum_rr              = config_get_int_value(config, "RR_QUANTUM");
    char *preemption_str = config_get_string_value(config, "QUEUE_PREEMPTION");
    int interrupcion_cola = (strcmp(preemption_str, "TRUE") == 0);
    int   timeout_suspension      = config_get_int_value(config, "SUSPENSION_TIMEOUT");

    /* Mostrar por pantalla */
    printf("\n========== CONFIGURACIÓN DEL KERNEL SCHEDULER ==========\n");
    printf("Algoritmo de planificación : %s\n", algoritmo_planificacion);
    printf("Cola de algoritmos         : %s\n", cola_algoritmos);
    printf("Quantum RR (ms)            : %d\n", quantum_rr);
    printf("Interrupción por cola      : %s\n", interrupcion_cola ? "TRUE" : "FALSE");
    printf("Timeout de suspensión (ms) : %d\n", timeout_suspension);
    printf("=======================================================\n\n");

    /* ===================================================== */

    /* ── Inicializar globales de planificación desde los valores leídos ── */
    if      (!strcmp(algoritmo_planificacion, "RR"))  g_algoritmo = ALGO_RR;
    else if (!strcmp(algoritmo_planificacion, "CMN")) g_algoritmo = ALGO_CMN;
    else                                               g_algoritmo = ALGO_FIFO;

    if (g_algoritmo == ALGO_CMN) {
        if (!ks_configurar_colas_cmn(cola_algoritmos)) {
            config_destroy(config);
            log_destroy(logger);
            return 1;
        }
    } else {
        ks_inicializar_colas_cmn();
    }

    g_quantum_ms    = quantum_rr;
    g_preemption    = (interrupcion_cola != 0);
    g_suspension_ms = timeout_suspension;

    log_info(logger, "Planificador: algoritmo=%s quantum=%d ms preemption=%s suspension=%d ms",
             algoritmo_planificacion, g_quantum_ms,
             g_preemption ? "TRUE" : "FALSE", g_suspension_ms);

    /* ================================================================ */

    /* Semáforo READY arranca en 0 — se señaliza cuando hay proceso listo */
    sem_init(&g_sem_ready, 0, 0);
    sem_init(&g_sem_io_request, 0, 0);

    pthread_t hilo_io_worker;
    if (pthread_create(&hilo_io_worker, NULL, ks_io_worker, NULL) != 0) {
        log_error(logger, "No se pudo crear el hilo worker de IO: %s", strerror(errno));
    } else {
        pthread_detach(hilo_io_worker);
    }

    log_info(logger, "Kernel Scheduler iniciando...");

    /* Conectar a KM */
    char *ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    
    
    /* ── Conectar a KM — UN SOLO SOCKET ── */
    fd_kernel_memory = conectar_a_servidor(logger, ip_km, puerto_km);
    if (fd_kernel_memory == -1) {
        log_error(logger, "Fallo conexion Kernel Memory");
        config_destroy(config); log_destroy(logger); return 1;
    }
    if (!enviar_handshake(logger, fd_kernel_memory, TIPO_KS)) {
        log_error(logger, "Handshake fallo con Kernel Memory");
        close(fd_kernel_memory);
        config_destroy(config); log_destroy(logger); return 1;
    }
    log_info(logger, "## Conectado a Kernel Memory (fd=%d)", fd_kernel_memory);

    /* Crear PID 0 */
    char *script = (argc > 2) ? argv[2] : "prueba";
    int32_t pid0 = 0, prio0 = 0;


    pthread_mutex_lock(&mutex_km_socket);
    t_paquete *paq = crear_paquete(OP_CREAR_PROCESO, logger);
    agregar_a_paquete(paq, &pid0,   sizeof(int32_t));
    agregar_a_paquete(paq, &prio0,  sizeof(int32_t));
    agregar_a_paquete(paq, script,  strlen(script));
    enviar_paquete(paq, fd_kernel_memory, logger);
    eliminar_paquete(paq);

    t_pcb *pcb0 = ks_crear_pcb(pid0, prio0);
    log_info(logger, "## (<0>) Se crea el proceso - Estado: NEW");

    int32_t resp_km;
    if (ks_recibir_de_km(&resp_km) && resp_km == OP_OK) // ← antes: recibir_int32
        log_info(logger, "KM confirmo creacion de PID 0");
    else
        log_warning(logger, "KM no confirmo creacion de PID 0");
    pthread_mutex_unlock(&mutex_km_socket);

    /* NEW → READY */
    ks_encolar_ready(pcb0);

    log_info(logger, "PID 0 en READY — script: %s, prioridad: %d", script, prio0);

    /* Servidor CPUs/IOs (en otro hilo) */
    printf("SISTEMA LISTO - Esperando CPUs/IOs (puerto %d)...\n",
           config_get_int_value(config, "PUERTO_ESCUCHA"));
    log_info(logger, "A la espera de conexiones CPUs/IOs");

    pthread_t hilo_srv;
    pthread_create(&hilo_srv, NULL, (void *)iniciar_servidor_puerto_escucha, NULL);
    pthread_join(hilo_srv, NULL);

    log_info(logger, "Kernel Scheduler finalizando...");
    sem_destroy(&g_sem_ready);
    sem_destroy(&g_sem_io_request);
    close(fd_kernel_memory);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}
