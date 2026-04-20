#include "kernel_s.h"
#include <cliente.h>
#include <server.h>
/*
int main(int argc, char* argv[]) {
    saludar("kernel_scheduler");
    return 0;
}
*/

t_log *logger = NULL;
t_config *config = NULL;


/* Fds de dispositivos IO — -1 si no están conectados */
int fd_io_sleep  = -1;
int fd_io_stdin  = -1;
int fd_io_stdout = -1;


/* =========================================================
 * ks_registrar_io
 * Guarda el fd del dispositivo IO según su subtipo.
 * ========================================================= */
 
void ks_registrar_io(int fd, int32_t subtipo)
{
    pthread_mutex_lock(&mutex_io);
    switch (subtipo)
    {
        case IO_SUBTIPO_SLEEP:
            if (fd_io_sleep != -1) close(fd_io_sleep);
            fd_io_sleep = fd;
            log_info(logger, "## Dispositivo IO SLEEP conectado (fd=%d)", fd);
            break;
        case IO_SUBTIPO_STDIN:
            if (fd_io_stdin != -1) close(fd_io_stdin);
            fd_io_stdin = fd;
            log_info(logger, "## Dispositivo IO STDIN conectado (fd=%d)", fd);
            break;
        case IO_SUBTIPO_STDOUT:
            if (fd_io_stdout != -1) close(fd_io_stdout);
            fd_io_stdout = fd;
            log_info(logger, "## Dispositivo IO STDOUT conectado (fd=%d)", fd);
            break;
        default:
            log_warning(logger, "Subtipo IO desconocido: %d (fd=%d)", subtipo, fd);
            close(fd);
            break;
    }
    pthread_mutex_unlock(&mutex_io);
}
 
/* =========================================================
 * ks_fd_io
 * Devuelve el fd del dispositivo IO pedido, o -1 si no hay.
 * ========================================================= */
 
int ks_fd_io(int32_t subtipo)
{
    switch (subtipo)
    {
        case IO_SUBTIPO_SLEEP:  return fd_io_sleep;
        case IO_SUBTIPO_STDIN:  return fd_io_stdin;
        case IO_SUBTIPO_STDOUT: return fd_io_stdout;
        default:                return -1;
    }
}
 
const char *ks_nombre_io(int32_t subtipo)
{
    switch (subtipo)
    {
        case IO_SUBTIPO_SLEEP:  return "SLEEP";
        case IO_SUBTIPO_STDIN:  return "STDIN";
        case IO_SUBTIPO_STDOUT: return "STDOUT";
        default:                return "DESCONOCIDO";
    }
}
 
/* =========================================================
 * ks_despachar_io_sleep
 * Envía syscall SLEEP al dispositivo y espera respuesta.
 * Protocolo KS → IO_SLEEP:
 *   OP_IO_SLEEP | PID (int32_t) | tiempo_ms (int32_t)
 * IO responde: OP_OK | OP_ERROR
 * ========================================================= */
 
bool ks_despachar_io_sleep(int32_t pid, int32_t tiempo_ms)
{
    pthread_mutex_lock(&mutex_io);
    int fd = fd_io_sleep;
    pthread_mutex_unlock(&mutex_io);
 
    if (fd == -1)
    {
        log_error(logger, "Dispositivo IO: SLEEP no encontrado");
        return false;
    }
 
    log_info(logger, "## (%d) - Solicitó syscall: SLEEP", pid);
    log_info(logger, "## (%d) Pasa del estado EXEC al estado BLOCK", pid);
 
    enviar_int32(fd, OP_IO_SLEEP);
    enviar_int32(fd, pid);
    enviar_int32(fd, tiempo_ms);
 
    int32_t resp;
    if (!recibir_int32(fd, &resp) || resp != OP_OK)
    {
        log_error(logger, "SLEEP — respuesta erronea del dispositivo (PID=%d)", pid);
        return false;
    }
 
    log_info(logger, "## (%d) finalizó IO y pasa a READY / SUSP. READY", pid);
    return true;
}
 
/* =========================================================
 * ks_despachar_io_stdin
 * Protocolo KS → IO_STDIN:
 *   OP_IO_STDIN | PID (int32_t) | cantidad_bytes (int32_t)
 * IO responde: OP_OK | cantidad (int32_t) | datos
 * KS luego escribe los datos en KM (dir_logica ya conocida).
 * ========================================================= */
 
bool ks_despachar_io_stdin(int32_t pid, int32_t cantidad)
{
    pthread_mutex_lock(&mutex_io);
    int fd = fd_io_stdin;
    pthread_mutex_unlock(&mutex_io);
 
    if (fd == -1)
    {
        log_error(logger, "Dispositivo IO: STDIN no encontrado");
        return false;
    }
 
    log_info(logger, "## (%d) - Solicitó syscall: STDIN", pid);
    log_info(logger, "## (%d) Pasa del estado EXEC al estado BLOCK", pid);
 
    enviar_int32(fd, OP_IO_STDIN);
    enviar_int32(fd, pid);
    enviar_int32(fd, cantidad);
 
    int32_t resp;
    if (!recibir_int32(fd, &resp) || resp != OP_OK)
    {
        log_error(logger, "STDIN — respuesta erronea del dispositivo (PID=%d)", pid);
        return false;
    }
 
    /* Recibir los datos leídos para luego enviarlos al KM */
    int32_t tam_recibido;
    if (!recibir_int32(fd, &tam_recibido))
    {
        log_error(logger, "STDIN — error recibiendo tamanio de datos (PID=%d)", pid);
        return false;
    }
 
    void *datos = malloc(tam_recibido);
    if (recv(fd, datos, tam_recibido, MSG_WAITALL) != tam_recibido)
    {
        log_error(logger, "STDIN — error recibiendo datos (PID=%d)", pid);
        free(datos);
        return false;
    }
 
    /* TODO: enviar 'datos' al KM con OP_ESCRIBIR_DATOS */
    log_info(logger, "## (%d) finalizó IO y pasa a READY / SUSP. READY", pid);
    free(datos);
    return true;
}
 
/* =========================================================
 * ks_despachar_io_stdout
 * Protocolo KS → IO_STDOUT:
 *   OP_IO_STDOUT | PID (int32_t) | tamanio (int32_t) | datos
 * IO responde: OP_OK | OP_ERROR
 * Los datos los obtiene el KS del KM antes de llamar a esta función.
 * ========================================================= */
 
bool ks_despachar_io_stdout(int32_t pid, void *datos, int32_t tamanio)
{
    pthread_mutex_lock(&mutex_io);
    int fd = fd_io_stdout;
    pthread_mutex_unlock(&mutex_io);
 
    if (fd == -1)
    {
        log_error(logger, "Dispositivo IO: STDOUT no encontrado");
        return false;
    }
 
    log_info(logger, "## (%d) - Solicitó syscall: STDOUT", pid);
    log_info(logger, "## (%d) Pasa del estado EXEC al estado BLOCK", pid);
 
    enviar_int32(fd, OP_IO_STDOUT);
    enviar_int32(fd, pid);
    enviar_int32(fd, tamanio);
    send(fd, datos, tamanio, MSG_NOSIGNAL);
 
    int32_t resp;
    if (!recibir_int32(fd, &resp) || resp != OP_OK)
    {
        log_error(logger, "STDOUT — respuesta erronea del dispositivo (PID=%d)", pid);
        return false;
    }
 
    log_info(logger, "## (%d) finalizó IO y pasa a READY / SUSP. READY", pid);
    return true;
}
 
/* =========================================================
 * ks_syscall_io
 * Punto de entrada genérico para syscalls de IO.
 * El subtipo indica qué dispositivo usar.
 * ========================================================= */
 
void ks_syscall_io(int32_t subtipo, int32_t pid, void *param, int32_t param_size)
{
    (void)param_size;
 
    if (ks_fd_io(subtipo) == -1)
    {
        log_error(logger, "Dispositivo IO: %s no encontrado",
                  ks_nombre_io(subtipo));
        return;
    }
 
    switch (subtipo)
    {
        case IO_SUBTIPO_SLEEP: {
            int32_t tiempo_ms = *((int32_t *)param);
            ks_despachar_io_sleep(pid, tiempo_ms);
            break;
        }
        case IO_SUBTIPO_STDIN: {
            int32_t cantidad = *((int32_t *)param);
            ks_despachar_io_stdin(pid, cantidad);
            break;
        }
        case IO_SUBTIPO_STDOUT: {
            /* param = datos ya obtenidos del KM, param_size = tamaño */
            ks_despachar_io_stdout(pid, param, param_size);
            break;
        }
        default:
            log_error(logger, "Subtipo IO desconocido: %d", subtipo);
            break;
    }
}
 
/* =========================================================
 * atender_cliente_ks
 * Hilo por conexión entrante al KS.
 * Identifica el tipo (CPU o IO) y despacha.
 * ========================================================= */
 
void *atender_cliente_ks(void *varg)
{
    t_hilo_arg *arg = (t_hilo_arg *)varg;
    int         fd  = arg->fd_cliente;
    free(arg);
 
    /*
     * recibir_handshake hace recv(tipo) + send(OK) en un paso.
     * El cliente (CPU o IO) está bloqueado en enviar_handshake
     * esperando ese OK — sin esto nunca avanza.
     */
    int32_t tipo = recibir_handshake(logger, fd);
    if (tipo == HANDSHAKE_ERR)
    {
        close(fd);
        return NULL;
    }
 
    if (tipo == TIPO_CPU)
    {
        int32_t id_cpu;
        if (!recibir_int32(fd, &id_cpu))
        {
            log_error(logger, "Error recibiendo ID de CPU (fd=%d)", fd);
            close(fd);
            return NULL;
        }
        log_info(logger, "## CPU %d Conectada (fd=%d)", id_cpu, fd);
 
        int32_t cod_op;
        while (recibir_int32(fd, &cod_op))
            log_info(logger, "CPU %d — cod_op: %d", id_cpu, cod_op);
 
        log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd);
        close(fd);
        return NULL;
    }
 
    if (tipo == TIPO_IO)
    {
        /* El dispositivo IO envía su subtipo post-handshake */
        int32_t subtipo;
        if (!recibir_int32(fd, &subtipo))
        {
            log_error(logger, "Error recibiendo subtipo IO (fd=%d)", fd);
            close(fd);
            return NULL;
        }
        ks_registrar_io(fd, subtipo);
 
        /*
         * El fd queda abierto y registrado.
         * El hilo termina — el KS usa el fd directamente
         * cuando necesita enviarle trabajo al dispositivo.
         * La desconexión del IO se detectará al enviarle
         * un pedido (send fallará con SIGPIPE / MSG_NOSIGNAL).
         */
        return NULL;
    }
 
    log_warning(logger, "Tipo desconocido: %d (fd=%d)", tipo, fd);
    close(fd);
    return NULL;
}
 
/* =========================================================
 * iniciar_servidor_puerto_escucha
 * Accept loop KS — CPUs e IOs.
 * ========================================================= */
 
void iniciar_servidor_puerto_escucha(void)
{
    int puerto = config_get_int_value(config, "PUERTO_ESCUCHA");
 
    int fd_escucha = iniciar_servidor(logger, puerto);
    if (fd_escucha == -1)
    {
        log_error(logger, "No se pudo iniciar servidor KS en puerto %d", puerto);
        return;
    }
 
    log_info(logger, "Kernel Scheduler esperando CPUs/IOs en puerto %d (fd=%d)",
             puerto, fd_escucha);
 
    while (1)
    {
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
    if (argc < 2)
    {
        fprintf(stderr, "Uso: %s kernel_s.config [nombre_script]\n", argv[0]);
        return 1;
    }
 
    logger = log_create("kernel_scheduler.log", "KERNEL_SCHEDULER", true, LOG_LEVEL_INFO);
    if (!logger) { fprintf(stderr, "Error creando logger\n"); return 1; }
 
    config = config_create(argv[1]);
    if (!config)
    {
        char alt[512];
        snprintf(alt, sizeof(alt), "../%s", argv[1]);
        config = config_create(alt);
    }
    if (!config)
    {
        log_error(logger, "No se pudo cargar '%s'", argv[1]);
        log_destroy(logger);
        return 1;
    }
 
    log_info(logger, "Kernel Scheduler iniciando...");
 
    /* ----- Conectar a Kernel Memory ----- */
    char *ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");
 
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_kernel_memory = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
 
    if (fd_kernel_memory == -1)
    {
        log_error(logger, "Fallo conexion Kernel Memory");
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    if (!enviar_handshake(logger, fd_kernel_memory, TIPO_KS))
    {
        log_error(logger, "Handshake fallo con Kernel Memory");
        close(fd_kernel_memory);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
    log_info(logger, "## Conectado a Kernel Memory (fd=%d)", fd_kernel_memory);
 
    /* ----- Enviar PID 0 al KM ----- */
    char *script_nombre = (argc > 2) ? argv[2] : "prueba";
    int32_t pid_inicial  = 0;
    int32_t prio_inicial = 0;
 
    t_paquete *paq = crear_paquete(OP_CREAR_PROCESO, logger);
    agregar_a_paquete(paq, &pid_inicial,  sizeof(int32_t));
    agregar_a_paquete(paq, &prio_inicial, sizeof(int32_t));
    agregar_a_paquete(paq, script_nombre, strlen(script_nombre));
    enviar_paquete(paq, fd_kernel_memory, logger);
    eliminar_paquete(paq);
 
    log_info(logger, "## (<0>) Se crea el proceso - Estado: NEW");
    log_info(logger, "PID 0 enviado a KM — script: %s, prioridad: %d",
             script_nombre, prio_inicial);
 
    /* ----- Servidor CPUs/IOs ----- */
    printf("SISTEMA LISTO - Esperando CPUs/IOs (puerto %d)...\n",
           config_get_int_value(config, "PUERTO_ESCUCHA"));
    log_info(logger, "A la espera de conexiones CPUs/IOs");
 
    pthread_t hilo_srv;
    pthread_create(&hilo_srv, NULL, (void *)iniciar_servidor_puerto_escucha, NULL);
    pthread_join(hilo_srv, NULL);
 
    log_info(logger, "Kernel Scheduler finalizando...");
    close(fd_kernel_memory);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}
