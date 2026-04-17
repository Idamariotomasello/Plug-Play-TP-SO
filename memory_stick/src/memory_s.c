#include "memory_s.h"
#include <cliente.h>
#include <server.h>

/* =========================================================
 * Globales del módulo
 * ========================================================= */
 
t_log    *logger = NULL;
t_config *config = NULL;
 
void   *g_memoria    = NULL;   /* bloque malloc'd del tamaño recibido */
int32_t g_tamanio    = 0;      /* tamaño en bytes */
int     g_memory_delay = 0;   /* MEMORY_DELAY en ms */
int     fd_km         = -1;   /* socket hacia Kernel Memory */
 
/* =========================================================
 * ms_leer
 * Lee g_memory_delay ms, luego copia 'tamanio' bytes
 * desde g_memoria[dir_fisica] al buffer de salida.
 * ========================================================= */
 
bool ms_leer(int32_t dir_fisica, int32_t tamanio, void *dest)
{
    if (dir_fisica < 0 || dir_fisica + tamanio > g_tamanio)
    {
        log_error(logger,
                  "Lectura fuera de rango: dir=%d tam=%d (ms_size=%d)",
                  dir_fisica, tamanio, g_tamanio);
        return false;
    }
 
    usleep(g_memory_delay * 1000);   /* MEMORY_DELAY en microsegundos */
 
    memcpy(dest, (char *)g_memoria + dir_fisica, tamanio);
    log_info(logger, "## Lectura de %d bytes", tamanio);
    return true;
}
 
/* =========================================================
 * ms_escribir
 * Espera g_memory_delay ms, luego copia 'tamanio' bytes
 * desde src a g_memoria[dir_fisica].
 * ========================================================= */
 
bool ms_escribir(int32_t dir_fisica, int32_t tamanio, void *src)
{
    if (dir_fisica < 0 || dir_fisica + tamanio > g_tamanio)
    {
        log_error(logger,
                  "Escritura fuera de rango: dir=%d tam=%d (ms_size=%d)",
                  dir_fisica, tamanio, g_tamanio);
        return false;
    }
 
    usleep(g_memory_delay * 1000);
 
    memcpy((char *)g_memoria + dir_fisica, src, tamanio);
    log_info(logger, "## Escritura de %d bytes", tamanio);
    return true;
}
 
/* =========================================================
 * ms_atender_cpu
 * Hilo permanente por CPU conectada.
 * Protocolo CPU → MS:
 *   cod_op (int32_t)
 *   OP_LEER_MS:     dir_fisica (int32_t) | tamanio (int32_t)
 *                   → responde: OP_OK | tamanio (int32_t) | datos
 *   OP_ESCRIBIR_MS: dir_fisica (int32_t) | tamanio (int32_t) | datos
 *                   → responde: OP_OK | OP_ERROR
 * ========================================================= */
 
void *ms_atender_cpu(void *varg)
{
    t_hilo_arg *arg    = (t_hilo_arg *)varg;
    int         fd_cpu = arg->fd_cliente;
    free(arg);
 
    log_info(logger, "## CPU Conectada (fd=%d)", fd_cpu);
 
    int32_t cod_op;
    while (recibir_int32(fd_cpu, &cod_op))
    {
        switch (cod_op)
        {
            case OP_LEER_MS: {
                int32_t dir, tam;
                if (!recibir_int32(fd_cpu, &dir) ||
                    !recibir_int32(fd_cpu, &tam))
                {
                    log_error(logger, "Error recibiendo parametros de lectura");
                    goto desconectar;
                }
 
                void *buf = malloc(tam);
                if (!buf || !ms_leer(dir, tam, buf))
                {
                    free(buf);
                    enviar_int32(fd_cpu, OP_ERROR);
                    break;
                }
 
                /* respuesta: OK + tamaño + datos */
                enviar_int32(fd_cpu, OP_OK);
                enviar_int32(fd_cpu, tam);
                send(fd_cpu, buf, tam, MSG_NOSIGNAL);
                free(buf);
                break;
            }
 
            case OP_ESCRIBIR_MS: {
                int32_t dir, tam;
                if (!recibir_int32(fd_cpu, &dir) ||
                    !recibir_int32(fd_cpu, &tam))
                {
                    log_error(logger, "Error recibiendo parametros de escritura");
                    goto desconectar;
                }
 
                void *buf = malloc(tam);
                if (!buf)
                {
                    enviar_int32(fd_cpu, OP_ERROR);
                    break;
                }
 
                if (recv(fd_cpu, buf, tam, MSG_WAITALL) != tam)
                {
                    free(buf);
                    enviar_int32(fd_cpu, OP_ERROR);
                    break;
                }
 
                bool ok = ms_escribir(dir, tam, buf);
                free(buf);
                enviar_int32(fd_cpu, ok ? OP_OK : OP_ERROR);
                break;
            }
 
            default:
                log_warning(logger, "CPU (fd=%d) — cod_op desconocido: %d",
                            fd_cpu, cod_op);
                break;
        }
    }
 
desconectar:
    log_info(logger, "CPU desconectada (fd=%d)", fd_cpu);
    close(fd_cpu);
    return NULL;
}
 
/* =========================================================
 * ms_iniciar_servidor_cpus
 * Accept loop para CPUs. Bloqueante — se lanza en hilo.
 * ========================================================= */
 
void ms_iniciar_servidor_cpus(int puerto)
{
    int fd_escucha = iniciar_servidor(logger, puerto);
    if (fd_escucha == -1)
    {
        log_error(logger, "No se pudo iniciar servidor MS en puerto %d", puerto);
        return;
    }
 
    log_info(logger, "Memory Stick esperando CPUs en puerto %d (fd=%d)",
             puerto, fd_escucha);
 
    while (1)
    {
        int fd_cpu = esperar_cliente(logger, fd_escucha);
        if (fd_cpu == -1) continue;
 
        /* El MS no hace handshake con la CPU — la CPU conoce
         * la dirección del MS a través del KM y conecta directo */
 
        t_hilo_arg *arg = malloc(sizeof(t_hilo_arg));
        if (!arg) { close(fd_cpu); continue; }
 
        arg->logger     = logger;
        arg->fd_cliente = fd_cpu;
 
        pthread_t hilo;
        pthread_create(&hilo, NULL, ms_atender_cpu, arg);
        pthread_detach(hilo);
    }
 
    close(fd_escucha);
}
 
 
void *hilo_servidor_cpus(void *varg)
{
    t_srv_arg *arg = (t_srv_arg *)varg;
    ms_iniciar_servidor_cpus(arg->puerto);
    free(arg);
    return NULL;
}
 
/* =========================================================
 * main
 * Uso: ./memory_stick memory_s.config <tamanio_en_bytes>
 * ========================================================= */
 
int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Uso: %s [archivo_config] [tamanio_bytes]\n", argv[0]);
        return 1;
    }
 
    /* ----- Logger ----- */
    logger = log_create("memory_stick.log", "MEMORY_STICK", true, LOG_LEVEL_INFO);
    if (!logger) { fprintf(stderr, "Error al crear logger\n"); return 1; }
 
    /* ----- Config ----- */
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
 
    /* ----- Tamaño por argumento ----- */
    g_tamanio = atoi(argv[2]);
    if (g_tamanio <= 0)
    {
        log_error(logger, "Tamanio invalido: %s", argv[2]);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    /* ----- Reservar memoria ----- */
    g_memoria = malloc(g_tamanio);
    if (!g_memoria)
    {
        log_error(logger, "No se pudo reservar %d bytes de memoria", g_tamanio);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
    memset(g_memoria, 0, g_tamanio);
    log_info(logger, "Memoria reservada: %d bytes", g_tamanio);
 
    /* ----- MEMORY_DELAY ----- */
    g_memory_delay = config_get_int_value(config, "MEMORY_DELAY");
    log_info(logger, "MEMORY_DELAY: %d ms", g_memory_delay);
 
    /* ----- Conectar a Kernel Memory ----- */
    char *ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");
 
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_km = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
 
    if (fd_km == -1)
    {
        log_error(logger, "Fallo conexion con Kernel Memory");
        free(g_memoria);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    /* Handshake TIPO_MS */
    if (!enviar_handshake(logger, fd_km, TIPO_MS))
    {
        log_error(logger, "Handshake fallo con Kernel Memory");
        close(fd_km);
        free(g_memoria);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    /* Enviar tamaño al KM — primer mensaje post-handshake */
    enviar_int32(fd_km, g_tamanio);
    log_info(logger, "## Conectado a Kernel Memory — tamanio enviado: %d bytes",
             g_tamanio);
 
    /* ----- Arrancar servidor de CPUs en hilo propio ----- */
    int puerto_escucha = config_get_int_value(config, "PUERTO_ESCUCHA");
 
    t_srv_arg *srv_arg = malloc(sizeof(t_srv_arg));
    srv_arg->puerto    = puerto_escucha;
 
    pthread_t hilo_srv;
    if (pthread_create(&hilo_srv, NULL, hilo_servidor_cpus, srv_arg) != 0)
    {
        log_error(logger, "Error al crear hilo servidor CPUs: %s", strerror(errno));
        free(srv_arg);
        close(fd_km);
        free(g_memoria);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    /*
     * El main hace join para que el proceso quede vivo.
     * El hilo del servidor de CPUs corre indefinidamente;
     * el hilo del KM (monitor de desconexión) podría agregarse
     * acá cuando se implemente la desconexión caliente.
     */
    pthread_join(hilo_srv, NULL);
 
    log_info(logger, "Memory Stick finalizando");
    close(fd_km);
    free(g_memoria);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}
