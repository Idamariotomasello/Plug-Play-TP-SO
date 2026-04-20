#include "io.h"
#include <cliente.h>
#include <server.h>

/* =========================================================
 * Globales del módulo
 * ========================================================= */
 
t_log    *logger = NULL;
t_config *config = NULL;
 
e_io_tipo g_tipo_io  = IO_TIPO_INVALIDO;
int       fd_ks      = -1;
 
/* =========================================================
 * io_ejecutar_sleep
 * Recibe tiempo_ms (int32_t) del KS, hace usleep, responde OK.
 * Protocolo KS → IO_SLEEP:
 *   OP_IO_SLEEP | PID (int32_t) | tiempo_ms (int32_t)
 * IO responde: OP_OK
 * ========================================================= */
 
void io_ejecutar_sleep(int32_t pid)
{
    int32_t tiempo_ms;
    if (!recibir_int32(fd_ks, &tiempo_ms))
    {
        log_error(logger, "SLEEP — error recibiendo tiempo (PID=%d)", pid);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
 
    log_info(logger, "## PID: %d - Inicio de IO", pid);
    log_info(logger, "## PID: %d - Haciendo sleep por %d milisegundos.", pid, tiempo_ms);
 
    usleep((useconds_t)tiempo_ms * 1000);
 
    log_info(logger, "## PID: %d - Fin de IO", pid);
    enviar_int32(fd_ks, OP_OK);
}
 
/* =========================================================
 * io_ejecutar_stdin
 * Recibe cantidad_bytes (int32_t) del KS.
 * Lee por teclado, ajusta al tamaño, responde con los datos.
 * Protocolo KS → IO_STDIN:
 *   OP_IO_STDIN | PID (int32_t) | cantidad_bytes (int32_t)
 * IO responde: OP_OK | cantidad_bytes (int32_t) | datos
 * ========================================================= */
 
void io_ejecutar_stdin(int32_t pid)
{
    int32_t cantidad;
    if (!recibir_int32(fd_ks, &cantidad))
    {
        log_error(logger, "STDIN — error recibiendo cantidad (PID=%d)", pid);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
 
    log_info(logger, "## PID: %d - Inicio de IO", pid);
    log_info(logger, "## PID: %d - Ingrese %d caracteres:", pid, cantidad);
 
    /* Leer línea del usuario */
    char *linea = NULL;
    size_t cap  = 0;
    ssize_t leidos = getline(&linea, &cap, stdin);
 
    /* Quitar \n si está presente */
    if (leidos > 0 && linea[leidos - 1] == '\n')
    {
        linea[leidos - 1] = '\0';
        leidos--;
    }
 
    /* Buffer de salida de exactamente 'cantidad' bytes */
    char *buf = calloc(cantidad, 1);   /* relleno con '\0' */
    if (leidos > 0)
    {
        int copiar = (leidos < cantidad) ? (int)leidos : cantidad;
        memcpy(buf, linea, copiar);
    }
    free(linea);
 
    log_info(logger, "## PID: %d - Fin de IO", pid);
 
    /* Responder: OP_OK + tamaño + datos */
    enviar_int32(fd_ks, OP_OK);
    enviar_int32(fd_ks, cantidad);
    send(fd_ks, buf, cantidad, MSG_NOSIGNAL);
    free(buf);
}
 
/* =========================================================
 * io_ejecutar_stdout
 * Recibe tamaño (int32_t) + datos del KS, imprime por pantalla.
 * Protocolo KS → IO_STDOUT:
 *   OP_IO_STDOUT | PID (int32_t) | tamanio (int32_t) | datos
 * IO responde: OP_OK
 * ========================================================= */
 
void io_ejecutar_stdout(int32_t pid)
{
    int32_t tamanio;
    if (!recibir_int32(fd_ks, &tamanio))
    {
        log_error(logger, "STDOUT — error recibiendo tamanio (PID=%d)", pid);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
 
    char *buf = malloc(tamanio + 1);
    if (!buf)
    {
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
 
    if (recv(fd_ks, buf, tamanio, MSG_WAITALL) != tamanio)
    {
        log_error(logger, "STDOUT — error recibiendo datos (PID=%d)", pid);
        free(buf);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
    buf[tamanio] = '\0';
 
    log_info(logger, "## PID: %d - Inicio de IO", pid);
    log_info(logger, "## PID: %d - %s", pid, buf);
    free(buf);
 
    log_info(logger, "## PID: %d - Fin de IO", pid);
    enviar_int32(fd_ks, OP_OK);
}
 
/* =========================================================
 * io_loop
 * Loop principal: recibe cod_op del KS y despacha.
 * ========================================================= */
 
void io_loop(void)
{
    const char *nombre = io_tipo_nombre(g_tipo_io);
    log_info(logger, "## Conectado a Kernel Scheduler — tipo: %s", nombre);
 
    int32_t cod_op;
    while (recibir_int32(fd_ks, &cod_op))
    {
        /* Todos los pedidos traen PID primero */
        int32_t pid;
        if (!recibir_int32(fd_ks, &pid))
        {
            log_error(logger, "%s — error recibiendo PID", nombre);
            break;
        }
 
        switch (cod_op)
        {
            case OP_IO_SLEEP:
                io_ejecutar_sleep(pid);
                break;
 
            case OP_IO_STDIN:
                io_ejecutar_stdin(pid);
                break;
 
            case OP_IO_STDOUT:
                io_ejecutar_stdout(pid);
                break;
 
            default:
                log_warning(logger, "%s — cod_op desconocido: %d", nombre, cod_op);
                enviar_int32(fd_ks, OP_ERROR);
                break;
        }
    }
 
    log_warning(logger, "Kernel Scheduler desconectado — %s finalizando", nombre);
}
 
/* =========================================================
 * io_tipo_nombre / io_tipo_desde_string
 * ========================================================= */
 
const char *io_tipo_nombre(e_io_tipo tipo)
{
    switch (tipo)
    {
        case IO_TIPO_SLEEP:  return "SLEEP";
        case IO_TIPO_STDIN:  return "STDIN";
        case IO_TIPO_STDOUT: return "STDOUT";
        default:             return "INVALIDO";
    }
}
 
e_io_tipo io_tipo_desde_string(const char *s)
{
    if (strcasecmp(s, "SLEEP")  == 0) return IO_TIPO_SLEEP;
    if (strcasecmp(s, "STDIN")  == 0) return IO_TIPO_STDIN;
    if (strcasecmp(s, "STDOUT") == 0) return IO_TIPO_STDOUT;
    return IO_TIPO_INVALIDO;
}
 
/* =========================================================
 * main
 * Uso: ./io io.config SLEEP|STDIN|STDOUT
 * ========================================================= */
 
int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Uso: %s [archivo_config] [SLEEP|STDIN|STDOUT]\n", argv[0]);
        return 1;
    }
 
    /* ----- Logger ----- */
    char log_name[64];
    snprintf(log_name, sizeof(log_name), "io_%s.log", argv[2]);
    logger = log_create(log_name, argv[2], true, LOG_LEVEL_INFO);
    if (!logger) { fprintf(stderr, "Error al crear logger\n"); return 1; }
 
    /* ----- Tipo de IO ----- */
    g_tipo_io = io_tipo_desde_string(argv[2]);
    if (g_tipo_io == IO_TIPO_INVALIDO)
    {
        log_error(logger, "Tipo de IO invalido: '%s'. Usar SLEEP, STDIN o STDOUT", argv[2]);
        log_destroy(logger);
        return 1;
    }
 
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
 
    /* ----- Conectar a Kernel Scheduler ----- */
    char *ip_ks     = config_get_string_value(config, "IP_KERNEL_SCHEDULER");
    int   puerto_ks = config_get_int_value(config, "PUERTO_KERNEL_SCHEDULER");
 
    log_info(logger, "Conectando a Kernel Scheduler %s:%d...", ip_ks, puerto_ks);
    fd_ks = conectar_a_servidor(logger, ip_ks, puerto_ks);
    free(ip_ks);
 
    if (fd_ks == -1)
    {
        log_error(logger, "Fallo conexion con Kernel Scheduler");
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    /* Handshake TIPO_IO — el KS necesita saber que es un IO */
    if (!enviar_handshake(logger, fd_ks, TIPO_IO))
    {
        log_error(logger, "Handshake fallo con Kernel Scheduler");
        close(fd_ks);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    /* Enviar subtipo de IO post-handshake */
    int32_t subtipo = (int32_t)g_tipo_io;
    enviar_int32(fd_ks, subtipo);
 
    log_info(logger, "## Conectado a Kernel Scheduler (tipo=%s, fd=%d)",
             io_tipo_nombre(g_tipo_io), fd_ks);
 
    /* ----- Loop de atención ----- */
    io_loop();
 
    close(fd_ks);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}
 