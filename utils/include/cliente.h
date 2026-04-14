#ifndef CLIENTE_H_
#define CLIENTE_H_

#include "conexion.h"


int crear_conexion(t_log* logger, char *ip, int puerto);
void crear_buffer(t_paquete* paquete);
t_paquete* crear_paquete(int cod_op, t_log* logger);
void agregar_a_paquete(t_paquete* paquete, void* valor, int tamanio);
void enviar_paquete(t_paquete* paquete, int socket_cliente);

bool enviar_int32(int fd, int32_t valor);

#endif
