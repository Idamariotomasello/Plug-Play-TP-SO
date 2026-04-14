#include "kernel_m.h"
#include <cliente.h>
#include <server.h>
/*
int main(int argc, char* argv[]) {
    saludar("kernel_memory");
    return 0;
}
*/
/* =========================================================
 * km_iniciar_servidor
 * Arranca el accept loop. Bloqueante — lanzar en hilo propio
 * si el main necesita hacer otras cosas en paralelo.
 * ========================================================= */
 
void km_iniciar_servidor(t_log *logger, int puerto)
{
    int fd_escucha = iniciar_servidor(logger, puerto);
    if (fd_escucha == -1)
    {
        log_error(logger, "No se pudo iniciar el servidor de Kernel Memory");
        return;
    }
 
    log_info(logger, "Kernel Memory esperando conexiones en puerto %d", puerto);
 
    while (1)
    {
        int fd_cliente = esperar_cliente(logger, fd_escucha);
        if (fd_cliente == -1)
            continue;
 
        /* Leer tipo de módulo y confirmar handshake */
        int32_t tipo = recibir_handshake(logger, fd_cliente);
        if (tipo == HANDSHAKE_ERR)
            continue;
 
        /* Empaquetar argumentos para el hilo */
        t_km_hilo_arg *arg = malloc(sizeof(t_km_hilo_arg));
        if (!arg)
        {
            log_error(logger, "Sin memoria para arg de hilo (fd=%d)", fd_cliente);
            close(fd_cliente);
            continue;
        }
 
        arg->logger     = logger;
        arg->fd_cliente = fd_cliente;
        arg->tipo       = tipo;
 
        pthread_t hilo;
        pthread_create(&hilo, NULL, km_despachar_cliente, arg);
        pthread_detach(hilo);
    }
 
    close(fd_escucha);
}
 
/* =========================================================
 * km_despachar_cliente
 * Hilo transitorio: decide a qué hilo atendedor permanente
 * derivar según el tipo recibido en el handshake.
 * ========================================================= */
 
void *km_despachar_cliente(void *varg)
{
    t_km_hilo_arg *arg    = (t_km_hilo_arg *)varg;
    t_log         *logger = arg->logger;
    int            fd     = arg->fd_cliente;
    int32_t        tipo   = arg->tipo;
    free(arg);
 
    switch (tipo)
    {
        case TIPO_KS:
            km_atender_ks(logger, fd);
            break;
 
        case TIPO_CPU:
            km_atender_cpu(logger, fd);
            break;
 
        case TIPO_MS:
            km_atender_ms(logger, fd);
            break;
 
        case TIPO_SWAP:
            km_atender_swap(logger, fd);
            break;
 
        default:
            log_warning(logger, "Tipo de módulo desconocido: %d (fd=%d) — cerrando", tipo, fd);
            close(fd);
            break;
    }
 
    return NULL;
}
 
/* =========================================================
 * km_atender_ks
 * Hilo permanente para el Kernel Scheduler.
 * ========================================================= */
 
void km_atender_ks(t_log *logger, int fd_ks)
{
    log_info(logger, "## Kernel Scheduler Conectado - FD del socket: %d", fd_ks);
 
    int32_t cod_op;
    while (recibir_int32(fd_ks, &cod_op))
    {
        log_info(logger, "KS — cod_op recibido: %d", cod_op);
        /* TODO: implementar operaciones en km_operaciones.c */
    }
 
    log_warning(logger, "Kernel Scheduler desconectado (fd=%d)", fd_ks);
    close(fd_ks);
}
 
/* =========================================================
 * km_atender_cpu
 * Hilo permanente para cada CPU conectada.
 * Recibe el ID de CPU como primer mensaje post-handshake.
 * ========================================================= */
 
void km_atender_cpu(t_log *logger, int fd_cpu)
{
    int32_t id_cpu;
    if (!recibir_int32(fd_cpu, &id_cpu))
    {
        log_error(logger, "Error al recibir ID de CPU (fd=%d)", fd_cpu);
        close(fd_cpu);
        return;
    }
 
    log_info(logger, "## CPU %d Conectada (fd=%d)", id_cpu, fd_cpu);
 
    int32_t cod_op;
    while (recibir_int32(fd_cpu, &cod_op))
    {
        log_info(logger, "CPU %d — cod_op recibido: %d", id_cpu, cod_op);
        /* TODO: implementar operaciones en km_operaciones.c */
    }
 
    log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd_cpu);
    close(fd_cpu);
}
 
/* =========================================================
 * km_atender_ms
 * Hilo monitor para cada Memory Stick.
 * Recibe el tamaño del MS como primer mensaje post-handshake.
 * Solo monitorea la conexión — el KM inicia todos los pedidos.
 * ========================================================= */
 
void km_atender_ms(t_log *logger, int fd_ms)
{
    int32_t tamanio;
    if (!recibir_int32(fd_ms, &tamanio))
    {
        log_error(logger, "Error al recibir tamaño de Memory Stick (fd=%d)", fd_ms);
        close(fd_ms);
        return;
    }
 
    log_info(logger, "## Memory Stick de %d bytes Conectada (fd=%d)", tamanio, fd_ms);
 
    /*
     * El MS es pasivo: el KM le envía pedidos directamente sobre fd_ms.
     * Este hilo solo detecta si el MS se cae inesperadamente.
     */
    char probe;
    if (recv(fd_ms, &probe, 1, MSG_WAITALL) <= 0)
        log_error(logger, "## Memory Stick (fd=%d) desconectado — memoria corrupta", fd_ms);
 
    close(fd_ms);
}
 
/* =========================================================
 * km_atender_swap
 * Hilo monitor para el módulo SWAP.
 * Recibe block_size y swap_size como primeros mensajes.
 * Solo monitorea la conexión — el KM inicia todos los pedidos.
 * ========================================================= */
 
void km_atender_swap(t_log *logger, int fd_swap)
{
    int32_t block_size, swap_size;
    if (!recibir_int32(fd_swap, &block_size) ||
        !recibir_int32(fd_swap, &swap_size))
    {
        log_error(logger, "Error al recibir metadatos de SWAP (fd=%d)", fd_swap);
        close(fd_swap);
        return;
    }
 
    log_info(logger, "## Conectado a SWAP (fd=%d) — bloque: %d bytes, total: %d bytes",
             fd_swap, block_size, swap_size);
 
    char probe;
    if (recv(fd_swap, &probe, 1, MSG_WAITALL) <= 0)
        log_error(logger, "SWAP (fd=%d) desconectado inesperadamente", fd_swap);
 
    close(fd_swap);
}
 
/* =========================================================
 * Argumento para el hilo del servidor
 * ========================================================= */
 
typedef struct
{
    t_log *logger;
    int    puerto;
} t_servidor_arg;
 
static void *hilo_servidor(void *varg)
{
    t_servidor_arg *arg = (t_servidor_arg *)varg;
    km_iniciar_servidor(arg->logger, arg->puerto);
    free(arg);
    return NULL;
}
 
/* =========================================================
 * main
 * ========================================================= */
 
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Uso: %s [archivo_config]\n", argv[0]);
        return 1;
    }
 
    /* ----- Logger ----- */
    t_log *logger = log_create("kernel_memory.log", "KERNEL_MEMORY", true, LOG_LEVEL_INFO);
    if (!logger)
    {
        fprintf(stderr, "Error al crear logger\n");
        return 1;
    }
 
    /* ----- Config ----- */
    /*
     * config_create() resuelve la ruta relativa al cwd del proceso.
     * Si se ejecuta desde bin/ y el config esta en el directorio
     * padre del modulo, se prueba tambien ../ automaticamente.
     */
    t_config *config = config_create(argv[1]);
    if (!config)
    {
        char ruta_alt[512];
        snprintf(ruta_alt, sizeof(ruta_alt), "../%s", argv[1]);
        config = config_create(ruta_alt);
    }
    if (!config)
    {
        log_error(logger, "No se pudo cargar la configuracion '%s'.", argv[1]);
        log_error(logger, "Ejecutar desde el directorio del modulo:");
        log_error(logger, "  cd kernel_memory && ./bin/kernel_memory kernel_m.config");
        log_destroy(logger);
        return 1;
    }
 
    int puerto = config_get_int_value(config, "PUERTO_ESCUCHA");
 
    log_info(logger, "Kernel Memory iniciando en puerto %d", puerto);
 
    /* ----- Lanzar accept loop en hilo propio ----- */
    t_servidor_arg *arg = malloc(sizeof(t_servidor_arg));
    if (!arg)
    {
        log_error(logger, "Sin memoria para arg del hilo servidor");
        config_destroy(config);
        log_destroy(logger);
        return 1;
    }
 
    arg->logger = logger;
    arg->puerto = puerto;
 
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, hilo_servidor, arg) != 0)
    {
        log_error(logger, "Error al crear hilo del servidor: %s", strerror(errno));
        free(arg);
        config_destroy(config);
        log_destroy(logger);
        return 1;
    }
 
    /*
     * El main hace join sobre el hilo del servidor para que
     * el proceso no termine mientras haya clientes conectados.
     * Si el accept loop muere (error irrecuperable), el proceso
     * termina limpiamente liberando recursos.
     */
    pthread_join(hilo, NULL);
 
    log_info(logger, "Kernel Memory finalizado");
 
    config_destroy(config);
    log_destroy(logger);
    return 0;
}