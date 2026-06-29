#include "memory_s.h"
#include <cliente.h>
#include <server.h>

/* Globales del módulo */
 
t_log    *logger = NULL;
t_config *config = NULL;
 
void   *g_memoria    = NULL;   /* bloque malloc'd del tamaño recibido */
int32_t g_tamanio    = 0;      /* tamaño en bytes */
int     g_memory_delay = 0;   /* MEMORY_DELAY en ms */
int     fd_km         = -1;   /* socket hacia Kernel Memory */
 

/* ms_leer
 * Lee g_memory_delay ms, luego copia 'tamanio' bytes
 * desde g_memoria[dir_fisica] al buffer de salida.
 */
 
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
 
/* ms_escribir
 * Espera g_memory_delay ms, luego copia 'tamanio' bytes
 * desde src a g_memoria[dir_fisica].
 */
 
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

/* ms_atender_km
 * Bucle principal que atiende los comandos del Kernel Memory.
 */
void ms_atender_km(void)
{
    int32_t cod_op;

    while (recibir_int32(fd_km, &cod_op)) {
        switch (cod_op) {

            case OP_LEER_MS: {    /* 700 */
                int32_t dir_fisica, tamanio;
                if (!recibir_int32(fd_km, &dir_fisica) ||
                    !recibir_int32(fd_km, &tamanio)) {
                    log_error(logger, "Error recibiendo parámetros de LEER_MS");
                    goto error_km;
                }

                log_info(logger, "## KM — LEER: dir_fisica=%d tam=%d",
                         dir_fisica, tamanio);

                void *buf = malloc(tamanio);
                if (!buf || !ms_leer(dir_fisica, tamanio, buf)) {
                    free(buf);
                    enviar_int32(fd_km, OP_ERROR);
                    log_error(logger,
                              "## KM — LEER FALLIDA: dir=%d tam=%d (ms_size=%d)",
                              dir_fisica, tamanio, g_tamanio);
                    break;
                }

                enviar_int32(fd_km, OP_OK);
                enviar_int32(fd_km, tamanio);
                send(fd_km, buf, tamanio, MSG_NOSIGNAL);
                free(buf);
                break;
            }

            case OP_ESCRIBIR_MS: {   /* 701 */
                int32_t dir_fisica, tamanio;
                if (!recibir_int32(fd_km, &dir_fisica) ||
                    !recibir_int32(fd_km, &tamanio)) {
                    log_error(logger, "Error recibiendo parámetros de ESCRIBIR_MS");
                    goto error_km;
                }

                log_info(logger, "## KM — ESCRIBIR: dir_fisica=%d tam=%d",
                         dir_fisica, tamanio);

                void *buf = malloc(tamanio);
                if (!buf) {
                    enviar_int32(fd_km, OP_ERROR);
                    break;
                }

                if (recv(fd_km, buf, tamanio, MSG_WAITALL) != tamanio) {
                    free(buf);
                    enviar_int32(fd_km, OP_ERROR);
                    log_error(logger,
                              "## KM — ESCRIBIR FALLIDA (recv): dir=%d tam=%d",
                              dir_fisica, tamanio);
                    break;
                }

                bool ok = ms_escribir(dir_fisica, tamanio, buf);
                free(buf);

                if (ok)
                    log_info(logger,
                             "## KM — ESCRIBIR OK: dir=%d tam=%d",
                             dir_fisica, tamanio);
                else
                    log_error(logger,
                              "## KM — ESCRIBIR FALLIDA: dir=%d tam=%d (ms_size=%d)",
                              dir_fisica, tamanio, g_tamanio);

                enviar_int32(fd_km, ok ? OP_OK : OP_ERROR);
                break;
            }

            default:
                log_warning(logger, "KM — cod_op desconocido: %d", cod_op);
                break;
        }
    }

error_km:
    log_warning(logger, "Kernel Memory desconectado (fd=%d)", fd_km);
    close(fd_km);
    fd_km = -1;
}
 
/* ms_atender_cpu
 * Hilo permanente por CPU conectada.
 * Protocolo CPU → MS:
 *   cod_op (int32_t)
 *   OP_LEER_MS:     dir_fisica (int32_t) | tamanio (int32_t)
 *                   → responde: OP_OK | tamanio (int32_t) | datos
 *   OP_ESCRIBIR_MS: dir_fisica (int32_t) | tamanio (int32_t) | datos
 *                   → responde: OP_OK | OP_ERROR
 */
 
void *ms_atender_cpu(void *varg)
{
    t_hilo_arg *arg    = (t_hilo_arg *)varg;
    int         fd_cpu = arg->fd_cliente;
    free(arg);
 
    /*
     * La CPU hace enviar_handshake(TIPO_CPU) antes de enviar su ID.
     * recibir_handshake hace recv(tipo) + send(OK) en un paso.
     */
    int32_t tipo = recibir_handshake(logger, fd_cpu);
    if (tipo == HANDSHAKE_ERR)
    {
        close(fd_cpu);
        return NULL;
    }
 
    /* Recibir ID de CPU enviado post-handshake */
    int32_t id_cpu;
    if (!recibir_int32(fd_cpu, &id_cpu))
    {
        log_error(logger, "Error recibiendo ID de CPU (fd=%d)", fd_cpu);
        close(fd_cpu);
        return NULL;
    }
 
    log_info(logger, "## CPU %d Conectada (fd=%d)", id_cpu, fd_cpu);
 
    int32_t cod_op;
    while (recibir_int32(fd_cpu, &cod_op))
    {
        switch (cod_op)
        {
            case OP_LEER_MS: {
                int32_t dir, tam;
                if (!recibir_int32(fd_cpu, &dir) || !recibir_int32(fd_cpu, &tam)) {
                    log_error(logger, "Error recibiendo parametros de lectura");
                    goto desconectar;
                }

                log_info(logger,
                        "## CPU %d — LEER: dir_fisica=%d tam=%d",
                        id_cpu, dir, tam);

                void *buf = malloc(tam);
                if (!buf || !ms_leer(dir, tam, buf)) {
                    free(buf);
                    log_error(logger,
                            "## CPU %d — LEER FALLIDA: dir=%d tam=%d (ms_size=%d)",
                            id_cpu, dir, tam, g_tamanio);
                    enviar_int32(fd_cpu, OP_ERROR);
                    break;
                }

                log_info(logger,
                        "## CPU %d — LEER OK: dir=%d tam=%d",
                        id_cpu, dir, tam);

                enviar_int32(fd_cpu, OP_OK);
                enviar_int32(fd_cpu, tam);
                send(fd_cpu, buf, tam, MSG_NOSIGNAL);
                free(buf);
                break;
            }

            case OP_ESCRIBIR_MS: {
                int32_t dir, tam;
                if (!recibir_int32(fd_cpu, &dir) || !recibir_int32(fd_cpu, &tam)) {
                    log_error(logger, "Error recibiendo parametros de escritura");
                    goto desconectar;
                }

                log_info(logger,
                        "## CPU %d — ESCRIBIR: dir_fisica=%d tam=%d",
                        id_cpu, dir, tam);

                void *buf = malloc(tam);
                if (!buf) { enviar_int32(fd_cpu, OP_ERROR); break; }

                if (recv(fd_cpu, buf, tam, MSG_WAITALL) != tam) {
                    free(buf);
                    enviar_int32(fd_cpu, OP_ERROR);
                    break;
                }

                bool ok = ms_escribir(dir, tam, buf);
                free(buf);

                if (ok)
                    log_info(logger,
                            "## CPU %d — ESCRIBIR OK: dir=%d tam=%d",
                            id_cpu, dir, tam);
                else
                    log_error(logger,
                            "## CPU %d — ESCRIBIR FALLIDA: dir=%d tam=%d (ms_size=%d)",
                            id_cpu, dir, tam, g_tamanio);

                enviar_int32(fd_cpu, ok ? OP_OK : OP_ERROR);
                break;
            }
 
            default:
            log_warning(logger, "CPU %d (fd=%d) — cod_op desconocido: %d (0x%X) — posible desincronización de protocolo",
                        id_cpu, fd_cpu, cod_op, cod_op);
            /* No cerrar — podría ser un mensaje fuera de orden; continuar leyendo */
            break;
        }
    }
 
desconectar:
    log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd_cpu);
    close(fd_cpu);
    return NULL;
}
 
/* ms_iniciar_servidor_cpus
 * Accept loop para CPUs. Bloqueante — se lanza en hilo.
 */
 
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
 
/* main
 * Uso: ./memory_stick memory_s.config <tamanio_en_bytes>
 */
 
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

    /* IP propia (leída del config o detectada) */
    char *ip_propia = config_get_string_value(config, "IP_ESCUCHA");
    if (!ip_propia) ip_propia = strdup("127.0.0.1");
    int32_t largo_ip = (int32_t)strlen(ip_propia);
    enviar_int32(fd_km, largo_ip);
    send(fd_km, ip_propia, largo_ip, MSG_NOSIGNAL);
    free(ip_propia);
    int puerto_escucha = config_get_int_value(config, "PUERTO_ESCUCHA");

    /* Puerto donde escucha CPUs */
    enviar_int32(fd_km, puerto_escucha);

    log_info(logger,
            "## Conectado a Kernel Memory — tamanio=%d bytes puerto_cpus=%d",
            g_tamanio, puerto_escucha);
            
 
    /* ----- Arrancar servidor de CPUs en hilo propio ----- */

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

    pthread_detach(hilo_srv);   // no esperamos a que termine
 
    /* El hilo principal atiende al Kernel Memory */
    ms_atender_km();

    /* Si ms_atender_km termina (desconexión del KM), finalizamos */
    log_info(logger, "Memory Stick finalizando");
    close(fd_km);
    free(g_memoria);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}

