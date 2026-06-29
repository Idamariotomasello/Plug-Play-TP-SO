/*
 * server.c
 * Funciones de red genéricas: iniciar servidor, esperar cliente,
 * conectarse a servidor, enviar y recibir primitivos.
 * Sin lógica de ningún módulo específico.
 */

#include "server.h"

/* iniciar_servidor
 * Crea socket, bind, listen. Devuelve fd_escucha o -1.
 */

int iniciar_servidor(t_log *logger, int puerto)
{
    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    char puerto_str[6];
    snprintf(puerto_str, sizeof(puerto_str), "%d", puerto);

    int status = getaddrinfo(NULL, puerto_str, &hints, &servinfo);
    if (status != 0)
    {
        log_error(logger, "getaddrinfo: %s", gai_strerror(status));
        return -1;
    }

    int fd = socket(servinfo->ai_family,
                    servinfo->ai_socktype,
                    servinfo->ai_protocol);
    if (fd == -1)
    {
        log_error(logger, "Error al crear socket servidor en puerto %d", puerto);
        freeaddrinfo(servinfo);
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    if (bind(fd, servinfo->ai_addr, servinfo->ai_addrlen) == -1)
    {
        log_error(logger, "Error en bind sobre puerto %d: %s", puerto, strerror(errno));
        close(fd);
        freeaddrinfo(servinfo);
        return -1;
    }

    freeaddrinfo(servinfo);

    if (listen(fd, SOMAXCONN) == -1)
    {
        log_error(logger, "Error en listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    log_info(logger, "Servidor escuchando en puerto %d (fd=%d)", puerto, fd);
    return fd;
}

/* esperar_cliente
 * Bloquea hasta que llega una conexión. Devuelve fd_cliente o -1.
 */

int esperar_cliente(t_log *logger, int fd_escucha)
{
    int fd_cliente = accept(fd_escucha, NULL, NULL);
    if (fd_cliente == -1)
        log_error(logger, "Error en accept: %s", strerror(errno));
    return fd_cliente;
}

/* conectar_a_servidor
 * Crea socket y conecta a ip:puerto. Devuelve fd o -1.
 */

int conectar_a_servidor(t_log *logger, const char *ip, int puerto)
{
    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char puerto_str[6];
    snprintf(puerto_str, sizeof(puerto_str), "%d", puerto);

    int status = getaddrinfo(ip, puerto_str, &hints, &servinfo);
    if (status != 0)
    {
        log_error(logger, "getaddrinfo (%s:%d): %s", ip, puerto, gai_strerror(status));
        return -1;
    }

    int fd = socket(servinfo->ai_family,
                    servinfo->ai_socktype,
                    servinfo->ai_protocol);
    if (fd == -1)
    {
        log_error(logger, "Error al crear socket cliente");
        freeaddrinfo(servinfo);
        return -1;
    }

    if (connect(fd, servinfo->ai_addr, servinfo->ai_addrlen) == -1)
    {
        log_error(logger, "Error al conectar a %s:%d: %s", ip, puerto, strerror(errno));
        close(fd);
        freeaddrinfo(servinfo);
        return -1;
    }

    freeaddrinfo(servinfo);
    log_info(logger, "Conectado a %s:%d (fd=%d)", ip, puerto, fd);
    return fd;
}

/* enviar_handshake / recibir_handshake
 * El cliente envía su tipo; el servidor responde OK o ERR.
 */

bool enviar_handshake(t_log *logger, int fd, int32_t tipo_modulo)
{
    if (send(fd, &tipo_modulo, sizeof(int32_t), MSG_NOSIGNAL) != sizeof(int32_t))
    {
        log_error(logger, "Error al enviar handshake (fd=%d)", fd);
        return false;
    }

    int32_t respuesta;
    if (recv(fd, &respuesta, sizeof(int32_t), MSG_WAITALL) != sizeof(int32_t))
    {
        log_error(logger, "Error al recibir respuesta de handshake (fd=%d)", fd);
        return false;
    }

    if (respuesta != HANDSHAKE_OK)
    {
        log_error(logger, "Handshake rechazado por servidor (fd=%d)", fd);
        return false;
    }

    log_info(logger, "Handshake OK (fd=%d, tipo=%d)", fd, tipo_modulo);
    return true;
}

int32_t recibir_handshake(t_log *logger, int fd)
{
    int32_t tipo;
    if (recv(fd, &tipo, sizeof(int32_t), MSG_WAITALL) != sizeof(int32_t))
    {
        log_warning(logger, "Handshake inválido (fd=%d) — cerrando", fd);
        close(fd);
        return HANDSHAKE_ERR;
    }

    int32_t ok = HANDSHAKE_OK;
    send(fd, &ok, sizeof(int32_t), MSG_NOSIGNAL);
    return tipo;
}

/* Primitivos de send / recv */

bool recibir_int32(int fd, int32_t *dest)
{
    return recv(fd, dest, sizeof(int32_t), MSG_WAITALL) == sizeof(int32_t);
}

void *recibir_buffer(int fd, int32_t *size_out)

{
    if (recv(fd, size_out, sizeof(int32_t), MSG_WAITALL) != sizeof(int32_t))
        return NULL;
    if (*size_out <= 0 || *size_out > MAX_BUFFER)
        return NULL;

    void *buf = malloc(*size_out);
    if (!buf)
        return NULL;

    if (recv(fd, buf, *size_out, MSG_WAITALL) != *size_out)
    {
        free(buf);
        return NULL;
    }
    return buf;
}
