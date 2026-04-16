#include "kernel_s.h"
#include <cliente.h>
#include <server.h>
/*
int main(int argc, char* argv[]) {
    saludar("kernel_scheduler");
    return 0;
}
*/

t_log    *logger = NULL;
t_config *config = NULL;

/* =========================================================
 * Hilo para atender CPUs/IOs conectadas al KS
 * ========================================================= */
 
void *atender_cliente_ks(void *varg)
{
    t_hilo_arg *arg    = (t_hilo_arg *)varg;
    int         fd     = arg->fd_cliente;
    free(arg);
 
    /* Leer tipo de módulo conectado */
    int32_t tipo;
    if (!recibir_int32(fd, &tipo)) {
        log_error(logger, "Error al recibir tipo de cliente (fd=%d)", fd);
        close(fd);
        return NULL;
    }
 
    switch (tipo) {
        case TIPO_CPU: {
            int32_t id_cpu;
            if (!recibir_int32(fd, &id_cpu)) {
                log_error(logger, "Error al recibir ID de CPU (fd=%d)", fd);
                close(fd);
                return NULL;
            }
            log_info(logger, "## CPU %d Conectada (fd=%d)", id_cpu, fd);
 
            int32_t cod_op;
            while (recibir_int32(fd, &cod_op))
                log_info(logger, "CPU %d — cod_op: %d", id_cpu, cod_op);
 
            log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd);
            break;
        }
        default:
            log_warning(logger, "Tipo desconocido: %d (fd=%d)", tipo, fd);
            break;
    }
 
    close(fd);
    return NULL;
}

/* =========================================================
 * Servidor multihilo KS — escucha CPUs e IOs
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
        if (!arg) {
            log_error(logger, "Sin memoria para arg (fd=%d)", fd_cliente);
            close(fd_cliente);
            continue;
        }
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
 
    /* ----- Logger ----- */
    logger = log_create("kernel_scheduler.log", "KERNEL_SCHEDULER", true, LOG_LEVEL_INFO);
    if (!logger) { fprintf(stderr, "Error creando logger\n"); return 1; }
 
    /* ----- Config ----- */
    config = config_create(argv[1]);
    if (!config) {
        char alt[512];
        snprintf(alt, sizeof(alt), "../%s", argv[1]);
        config = config_create(alt);
    }
    if (!config) {
        log_error(logger, "No se pudo cargar '%s'", argv[1]);
        log_destroy(logger);
        return 1;
    }
 
    log_info(logger, "Kernel Scheduler iniciando...");
 
    /* ----- Conectar a Kernel Memory ----- */
    char *ip_km        = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km    = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");
 
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_kernel_memory = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
 
    if (fd_kernel_memory == -1) {
        log_error(logger, "Fallo conexion Kernel Memory");
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    /* Handshake — TIPO_KS = 12 */
    if (!enviar_handshake(logger, fd_kernel_memory, TIPO_KS)) {
        log_error(logger, "Handshake fallo con Kernel Memory");
        close(fd_kernel_memory);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
    log_info(logger, "## Conectado a Kernel Memory (fd=%d)", fd_kernel_memory);
 
    /* ----- Enviar OP_CREAR_PROCESO (PID=0, prioridad=0, script) ----- */
    char *script_nombre = (argc > 2) ? argv[2] : "prueba";
 
    int32_t pid_inicial  = 0;
    int32_t prio_inicial = 0;   /* prioridad maxima */
 
    t_paquete *paq = crear_paquete(OP_CREAR_PROCESO, logger);
    agregar_a_paquete(paq, &pid_inicial,  sizeof(int32_t));
    agregar_a_paquete(paq, &prio_inicial, sizeof(int32_t));
    agregar_a_paquete(paq, script_nombre, strlen(script_nombre));
    enviar_paquete(paq, fd_kernel_memory, logger);
    eliminar_paquete(paq);
 
    log_info(logger, "## (<0>) Se crea el proceso - Estado: NEW");
    log_info(logger, "PID 0 enviado a KM — script: %s, prioridad: %d",
             script_nombre, prio_inicial);
 
    /* ----- Arrancar servidor de CPUs/IOs en hilo propio ----- */
    printf("SISTEMA LISTO - Esperando conexiones CPUs (puerto %d)...\n",
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

