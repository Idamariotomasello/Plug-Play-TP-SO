#include "kernel_s.h"
#include <cliente.h>
#include <server.h>

/* =========================================================
 * Globales del módulo
 * ========================================================= */

t_log    *logger          = NULL;
t_config *config          = NULL;
int       fd_kernel_memory = -1;

/* Dispositivos IO */
int             fd_io_sleep  = -1;
int             fd_io_stdin  = -1;
int             fd_io_stdout = -1;
pthread_mutex_t mutex_io     = PTHREAD_MUTEX_INITIALIZER;

/* PCBs del planificador */
t_pcb        g_ks_pcbs[KS_MAX_PROCESOS];
pthread_mutex_t g_mutex_ks_pcbs = PTHREAD_MUTEX_INITIALIZER;

/* Semáforo: hay proceso READY para despachar a una CPU */
sem_t g_sem_ready;

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

static t_pcb *ks_crear_pcb(int32_t pid, int32_t prioridad)
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

static t_pcb *ks_buscar_pcb(int32_t pid)
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
static t_pcb *ks_primer_ready(void)
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

/* =========================================================
 * Despacho a dispositivos IO
 * ========================================================= */

bool ks_despachar_io_sleep(int32_t pid, int32_t tiempo_ms)
{
    pthread_mutex_lock(&mutex_io);
    int fd = fd_io_sleep;
    pthread_mutex_unlock(&mutex_io);

    if (fd == -1) {
        log_error(logger, "Dispositivo IO: SLEEP no encontrado");
        return false;
    }

    log_info(logger, "## (%d) - Solicitó syscall: SLEEP", pid);

    t_pcb *pcb = ks_buscar_pcb(pid);
    if (pcb) ks_cambiar_estado(pcb, ESTADO_BLOCK);

    enviar_int32(fd, OP_IO_SLEEP);
    enviar_int32(fd, pid);
    enviar_int32(fd, tiempo_ms);

    int32_t resp;
    if (!recibir_int32(fd, &resp) || resp != OP_OK) {
        log_error(logger, "SLEEP — respuesta erronea (PID=%d)", pid);
        return false;
    }

    log_info(logger, "## (%d) finalizó IO y pasa a READY / SUSP. READY", pid);
    if (pcb) {
        ks_cambiar_estado(pcb, ESTADO_READY);
        sem_post(&g_sem_ready);
    }
    return true;
}

bool ks_despachar_io_stdin(int32_t pid, int32_t cantidad)
{
    pthread_mutex_lock(&mutex_io);
    int fd = fd_io_stdin;
    pthread_mutex_unlock(&mutex_io);

    if (fd == -1) {
        log_error(logger, "Dispositivo IO: STDIN no encontrado");
        return false;
    }

    log_info(logger, "## (%d) - Solicitó syscall: STDIN", pid);

    t_pcb *pcb = ks_buscar_pcb(pid);
    if (pcb) ks_cambiar_estado(pcb, ESTADO_BLOCK);

    enviar_int32(fd, OP_IO_STDIN);
    enviar_int32(fd, pid);
    enviar_int32(fd, cantidad);

    int32_t resp;
    if (!recibir_int32(fd, &resp) || resp != OP_OK) {
        log_error(logger, "STDIN — respuesta erronea (PID=%d)", pid);
        return false;
    }

    int32_t tam;
    if (!recibir_int32(fd, &tam)) {
        log_error(logger, "STDIN — error recibiendo tamanio (PID=%d)", pid);
        return false;
    }
    void *datos = malloc(tam);
    if (recv(fd, datos, tam, MSG_WAITALL) != tam) {
        log_error(logger, "STDIN — error recibiendo datos (PID=%d)", pid);
        free(datos);
        return false;
    }
    /* TODO checkpoint 3: enviar datos al KM con OP_ESCRIBIR_DATOS */
    log_info(logger, "## (%d) finalizó IO y pasa a READY / SUSP. READY", pid);
    free(datos);
    if (pcb) {
        ks_cambiar_estado(pcb, ESTADO_READY);
        sem_post(&g_sem_ready);
    }
    return true;
}

bool ks_despachar_io_stdout(int32_t pid, void *datos, int32_t tamanio)
{
    pthread_mutex_lock(&mutex_io);
    int fd = fd_io_stdout;
    pthread_mutex_unlock(&mutex_io);

    if (fd == -1) {
        log_error(logger, "Dispositivo IO: STDOUT no encontrado");
        return false;
    }

    log_info(logger, "## (%d) - Solicitó syscall: STDOUT", pid);

    t_pcb *pcb = ks_buscar_pcb(pid);
    if (pcb) ks_cambiar_estado(pcb, ESTADO_BLOCK);

    enviar_int32(fd, OP_IO_STDOUT);
    enviar_int32(fd, pid);
    enviar_int32(fd, tamanio);
    send(fd, datos, tamanio, MSG_NOSIGNAL);

    int32_t resp;
    if (!recibir_int32(fd, &resp) || resp != OP_OK) {
        log_error(logger, "STDOUT — respuesta erronea (PID=%d)", pid);
        return false;
    }

    log_info(logger, "## (%d) finalizó IO y pasa a READY / SUSP. READY", pid);
    if (pcb) {
        ks_cambiar_estado(pcb, ESTADO_READY);
        sem_post(&g_sem_ready);
    }
    return true;
}

void ks_syscall_io(int32_t subtipo, int32_t pid, void *param, int32_t param_size)
{
    if (ks_fd_io(subtipo) == -1) {
        log_error(logger, "Dispositivo IO: %s no encontrado", ks_nombre_io(subtipo));
        return;
    }
    switch (subtipo) {
        case OP_IO_SLEEP: {
            int32_t t = *((int32_t *)param);
            ks_despachar_io_sleep(pid, t);
            break;
        }
        case OP_IO_STDIN: {
            int32_t c = *((int32_t *)param);
            ks_despachar_io_stdin(pid, c);
            break;
        }
        case OP_IO_STDOUT:
            ks_despachar_io_stdout(pid, param, param_size);
            break;
        default:
            log_error(logger, "Subtipo IO desconocido: %d", subtipo);
    }
}

/* =========================================================
 * Procesamiento de syscall recibida desde la CPU
 * Parsea la instrucción del PCB y despacha al dispositivo correcto.
 * ========================================================= */

static void ks_procesar_syscall(t_pcb *pcb)
{
    const char *op  = pcb->syscall_nombre;
    const char *a1  = pcb->syscall_arg1;

    log_info(logger, "## (%d) - Solicitó syscall: %s", pcb->pid, op);

    if (!strcmp(op, "SLEEP")) {
        int32_t ms = atoi(a1);
        ks_despachar_io_sleep(pcb->pid, ms);

    } else if (!strcmp(op, "STDIN")) {
        /* STDIN CX DX — CX tiene dirección lógica, DX tamaño */
        /* Por ahora enviamos tamaño fijo del registro DX — TODO: leer del PCB */
        int32_t cantidad = 8;
        ks_despachar_io_stdin(pcb->pid, cantidad);

    } else if (!strcmp(op, "STDOUT")) {
        /* STDOUT AX BX — AX dir lógica, BX tamaño */
        /* TODO: leer datos del KM antes de enviar */
        char mock_datos[] = "(datos pendientes KM)";
        ks_despachar_io_stdout(pcb->pid, mock_datos, strlen(mock_datos));

    } else if (!strcmp(op, "EXIT")) {
        ks_cambiar_estado(pcb, ESTADO_EXIT);
        log_info(logger, "## (%d) finalizó su ejecución con motivo de EXIT", pcb->pid);

    } else if (!strcmp(op, "MUTEX_CREATE") || !strcmp(op, "MUTEX_LOCK") ||
               !strcmp(op, "MUTEX_UNLOCK") || !strcmp(op, "MEM_ALLOC")  ||
               !strcmp(op, "MEM_FREE")     || !strcmp(op, "INIT_PROC")) {
        /* TODO checkpoint 2/3 */
        log_info(logger, "## (%d) - syscall %s pendiente de implementacion", pcb->pid, op);
        ks_cambiar_estado(pcb, ESTADO_READY);
        sem_post(&g_sem_ready);

    } else {
        log_warning(logger, "## (%d) - syscall desconocida: %s", pcb->pid, op);
        ks_cambiar_estado(pcb, ESTADO_READY);
        sem_post(&g_sem_ready);
    }
}

/* =========================================================
 * atender_cliente_ks
 * Hilo por conexión: identifica tipo (CPU / IO) y actúa.
 * Para la CPU: planificador simplificado FIFO.
 * ========================================================= */

void *atender_cliente_ks(void *varg)
{
    t_hilo_arg *arg = (t_hilo_arg *)varg;
    int         fd  = arg->fd_cliente;
    free(arg);

    int32_t tipo = recibir_handshake(logger, fd);
    if (tipo == HANDSHAKE_ERR) { close(fd); return NULL; }

    /* ── CPU ── */
    if (tipo == TIPO_CPU) {
        int32_t id_cpu;
        if (!recibir_int32(fd, &id_cpu)) {
            log_error(logger, "Error recibiendo ID de CPU (fd=%d)", fd);
            close(fd); return NULL;
        }
        log_info(logger, "## CPU %d Conectada (fd=%d)", id_cpu, fd);

        /*
         * Loop planificador:
         *  1. Esperar que haya un proceso READY (sem_wait)
         *  2. Cambiar a EXEC y enviar PID a la CPU
         *  3. Esperar devolución (pid + motivo)
         *  4. Según motivo: EXIT, SYSCALL o SEG_FAULT
         */
        while (1)
        {
            /* Bloquear hasta que haya proceso READY */
            log_info(logger, "CPU %d esperando proceso READY...", id_cpu);
            sem_wait(&g_sem_ready);

            t_pcb *pcb = ks_primer_ready();
            if (!pcb) {
                /* señal espuria — devolver al semáforo */
                log_warning(logger, "CPU %d: sem_wait desbloqueado sin READY", id_cpu);
                continue;
            }

            ks_cambiar_estado(pcb, ESTADO_EXEC);
            log_info(logger, "## CPU %d — enviando PID %d a ejecutar", id_cpu, pcb->pid);

            /* Enviar PID a la CPU */
            enviar_int32(fd, pcb->pid);

            /* Esperar devolución */
            int32_t pid_ret, motivo;
            if (!recibir_int32(fd, &pid_ret) || !recibir_int32(fd, &motivo)) {
                log_error(logger, "CPU %d — error recibiendo devolución", id_cpu);
                ks_cambiar_estado(pcb, ESTADO_READY);
                sem_post(&g_sem_ready);
                break;
            }

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

                log_info(logger, "## CPU %d — syscall recibida: '%s' arg1='%s' arg2='%s' vals=%d,%d",
                        id_cpu, pcb->syscall_nombre, pcb->syscall_arg1, pcb->syscall_arg2, val1, val2);
            }

            const char *nombre_motivo;
            if (motivo == KS_MOTIVO_EXIT) nombre_motivo = "EXIT";
            else if (motivo == KS_MOTIVO_SYSCALL) nombre_motivo = "SYSCALL";
            else if (motivo == KS_MOTIVO_INTERRUPCION) nombre_motivo = "INTERRUPCION";
            else if (motivo == KS_MOTIVO_SEG_FAULT) nombre_motivo = "SEG_FAULT";
            else nombre_motivo = "DESCONOCIDO";
            
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
                    ks_cambiar_estado(pcb, ESTADO_READY);
                    sem_post(&g_sem_ready);
                    break;


                case KS_MOTIVO_NINGUNO:   /* ciclo normal, ej: NOOP */
                    log_info(logger, "## (%d) - CPU completó ciclo sin evento (NOOP u op normal)",
                            pid_ret);
                    ks_cambiar_estado(pcb, ESTADO_READY);
                    sem_post(&g_sem_ready);
                    break;

                default:
                    log_warning(logger, "Motivo desconocido: %d", motivo);
                    ks_cambiar_estado(pcb, ESTADO_READY);
                    sem_post(&g_sem_ready);
                    break;
            }
        }

        log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd);
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

    /* Semáforo READY arranca en 0 — se señaliza cuando hay proceso listo */
    sem_init(&g_sem_ready, 0, 0);

    log_info(logger, "Kernel Scheduler iniciando...");

    /* Conectar a KM */
    char *ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_kernel_memory = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
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

    t_paquete *paq = crear_paquete(OP_CREAR_PROCESO, logger);
    agregar_a_paquete(paq, &pid0,   sizeof(int32_t));
    agregar_a_paquete(paq, &prio0,  sizeof(int32_t));
    agregar_a_paquete(paq, script,  strlen(script));
    enviar_paquete(paq, fd_kernel_memory, logger);
    eliminar_paquete(paq);

    /* PCB PID 0 en el planificador */
    t_pcb *pcb0 = ks_crear_pcb(pid0, prio0);
    log_info(logger, "## (<0>) Se crea el proceso - Estado: NEW");

    /* KM responde OK al OP_CREAR_PROCESO */
    int32_t resp_km;
    if (recibir_int32(fd_kernel_memory, &resp_km) && resp_km == OP_OK)
        log_info(logger, "KM confirmo creacion de PID 0");
    else
        log_warning(logger, "KM no confirmo creacion de PID 0");

    /* NEW → READY */
    ks_cambiar_estado(pcb0, ESTADO_READY);
    sem_post(&g_sem_ready);   /* señalizar que hay un proceso listo */

    log_info(logger, "PID 0 en READY — script: %s, prioridad: %d", script, prio0);

    /* Servidor CPUs/IOs */
    printf("SISTEMA LISTO - Esperando CPUs/IOs (puerto %d)...\n",
           config_get_int_value(config, "PUERTO_ESCUCHA"));
    log_info(logger, "A la espera de conexiones CPUs/IOs");

    pthread_t hilo_srv;
    pthread_create(&hilo_srv, NULL, (void *)iniciar_servidor_puerto_escucha, NULL);
    pthread_join(hilo_srv, NULL);

    log_info(logger, "Kernel Scheduler finalizando...");
    sem_destroy(&g_sem_ready);
    close(fd_kernel_memory);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}
