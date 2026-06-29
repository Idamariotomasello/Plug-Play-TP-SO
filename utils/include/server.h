#ifndef SERVER_H_
#define SERVER_H_

#include "conexion.h"

/* Constantes globales */
 
#define MAX_BUFFER    (10 * 1024 * 1024)  /* 10 MB */
 
#define HANDSHAKE_OK   0
#define HANDSHAKE_ERR -1


typedef struct
{
    t_log *logger;
    int    puerto;
} t_servidor_arg;
 
 
/* API genérica de red */
 
/* Servidor */
int     iniciar_servidor(t_log *logger, int puerto);
int     esperar_cliente(t_log *logger, int fd_escucha);
 
/* Cliente */
int     conectar_a_servidor(t_log *logger, const char *ip, int puerto);
 
/* Handshake */
bool    enviar_handshake(t_log *logger, int fd, int32_t tipo_modulo);
int32_t recibir_handshake(t_log *logger, int fd);
 
/* Primitivos */
bool    recibir_int32(int fd, int32_t *dest);
void   *recibir_buffer(int fd, int32_t *size_out);
 
#endif /* SERVER_H */
