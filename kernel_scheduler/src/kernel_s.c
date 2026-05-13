#include "kernel_s.h"
#include <cliente.h>
#include <server.h>

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
        ks_encolar_ready(pcb);
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
        ks_encolar_ready(pcb);
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
        ks_encolar_ready(pcb);
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
 * ks_recibir_de_km
 * Lee un int32 del socket KM descartando cualquier
 * OP_NUEVA_MEMORIA que llegue antes de la respuesta real.
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

    } else if (!strcmp(op, "INIT_PROC")) {

        const char *nombre_script = pcb->syscall_arg1;
        const char *prio_str      = pcb->syscall_arg2;
        int32_t     prioridad     = atoi(prio_str);

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
                        "verificar si hijo PID %d (prio=%d) desaloja proceso en EXEC",
                        pcb->pid, nuevo_pid, prioridad);
                /* TODO: enviar interrupción a CPU si hay proceso en EXEC con menor prioridad */
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
    
    } else if (!strcmp(op, "MUTEX_CREATE") || !strcmp(op, "MUTEX_LOCK") ||
               !strcmp(op, "MUTEX_UNLOCK") ||
               !strcmp(op, "MEM_FREE")) {
        /* TODO checkpoint 2/3 */
        log_info(logger, "## (%d) - syscall %s pendiente de implementacion", pcb->pid, op);
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
            log_info(logger, "## CPU %d — enviando PID %d a ejecutar", id_cpu, pcb->pid);

            /* Enviar PID a la CPU */
            enviar_int32(fd, pcb->pid);

            /* Esperar devolución */
            int32_t pid_ret, motivo;
            if (!recibir_int32(fd, &pid_ret) || !recibir_int32(fd, &motivo)) {
                log_error(logger, "CPU %d — error recibiendo devolución", id_cpu);
                ks_encolar_ready(pcb); /* reencolar para intentar ejecutar en otra CPU */
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
            log_info(logger,
                     "## (%d) — [CMN] encolando por prioridad %d "
                     "(QUEUE_PREEMPTION=%s)",
                     pcb->pid, pcb->prioridad,
                     g_preemption ? "TRUE" : "FALSE");
            cola_ready_encolar_cmn(pcb);
            break;
    }

    sem_post(&g_sem_ready);
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

    g_quantum_ms    = quantum_rr;
    g_preemption    = (interrupcion_cola != 0);
    g_suspension_ms = timeout_suspension;

    log_info(logger, "Planificador: algoritmo=%s quantum=%d ms preemption=%s suspension=%d ms",
             algoritmo_planificacion, g_quantum_ms,
             g_preemption ? "TRUE" : "FALSE", g_suspension_ms);

    free(algoritmo_planificacion);
    free(cola_algoritmos);
    free(preemption_str);
    /* ================================================================ */

    /* Semáforo READY arranca en 0 — se señaliza cuando hay proceso listo */
    sem_init(&g_sem_ready, 0, 0);

    log_info(logger, "Kernel Scheduler iniciando...");

    /* Conectar a KM */
    char *ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    
    
    /* ── Conectar a KM — UN SOLO SOCKET ── */
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
    close(fd_kernel_memory);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}
