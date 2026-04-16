#include "cliente.h"



int crear_conexion(t_log* logger, char *ip, int puerto)
{
	log_info(logger, "Creando conexion con el servidor %s:%d", ip, puerto);

	struct addrinfo hints;
	struct addrinfo *server_info;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	char puerto_char[6];
	sprintf(puerto_char, "%d", puerto); // convierte int a string

	getaddrinfo(ip, puerto_char, &hints, &server_info);

	// Ahora vamos a crear el socket.
	//int socket_cliente = 0;
	
	int socket_cliente = socket(server_info->ai_family,
								server_info->ai_socktype,
								server_info->ai_protocol);

	if (socket_cliente == -1) {
		perror("Error al crear socket");
		freeaddrinfo(server_info);
		return -1;
	}
	
	// Ahora que tenemos el socket, vamos a conectarlo

	log_info(logger, "Conectando al socket %d", socket_cliente);
	
	if (connect(socket_cliente, server_info->ai_addr, server_info->ai_addrlen) == -1) {
		perror("Error al conectar con el servidor");
		close(socket_cliente);
		freeaddrinfo(server_info);
		return -1;
	}
	

	freeaddrinfo(server_info);

	return socket_cliente;
}



bool enviar_int32(int fd, int32_t valor)
{
    return send(fd, &valor, sizeof(int32_t), MSG_NOSIGNAL) == sizeof(int32_t);
}


void crear_buffer(t_paquete* paquete)
{
	paquete->buffer = malloc(sizeof(t_buffer));
	paquete->buffer->size = 0;
	paquete->buffer->stream = NULL;
}

t_paquete* crear_paquete(int cod_op, t_log* logger)
{
	log_info(logger, "Creando paquete con código de operación: %d", cod_op);
	t_paquete* paquete = malloc(sizeof(t_paquete));
	paquete->codigo_operacion = cod_op;
	crear_buffer(paquete);
	if (paquete->buffer == NULL) {
		log_error(logger, "Error al crear el buffer del paquete");
		free(paquete);
		return NULL;
	}
	return paquete;
}

void agregar_a_paquete(t_paquete* paquete, void* valor, int tamanio)
{
	paquete->buffer->stream = realloc(paquete->buffer->stream, paquete->buffer->size + tamanio + sizeof(int));

	memcpy(paquete->buffer->stream + paquete->buffer->size, &tamanio, sizeof(int));
	memcpy(paquete->buffer->stream + paquete->buffer->size + sizeof(int), valor, tamanio);

	paquete->buffer->size += tamanio + sizeof(int);
}

void enviar_paquete(t_paquete* paquete, int socket_cliente,  t_log* logger) {
 
	    // Validaciones
    if (paquete == NULL) {
        if (logger) log_error(logger, "Error: paquete es NULL");
        return;
    }
    
    if (paquete->buffer == NULL) {
        if (logger) log_error(logger, "Error: buffer del paquete es NULL");
        return;
    }
    
    if (socket_cliente <= 0) {
        if (logger) log_error(logger, "Error: socket inválido: %d", socket_cliente);
        return;
    }
    
    // 1. Enviar código de operación
    ssize_t bytes_enviados = send(socket_cliente, &(paquete->codigo_operacion), sizeof(int), 0);
    if (bytes_enviados != sizeof(int)) {
        if (logger) log_error(logger, "Error al enviar código de operación. Enviados: %zd, esperados: %zu", bytes_enviados, sizeof(int));
        return;
    }
    
    // 2. Enviar tamaño del buffer
    bytes_enviados = send(socket_cliente, &(paquete->buffer->size), sizeof(int), 0);
    if (bytes_enviados != sizeof(int)) {
        if (logger) log_error(logger, "Error al enviar tamaño del buffer. Enviados: %zd, esperados: %zu", bytes_enviados, sizeof(int));
        return;
    }
    
    // 3. Enviar contenido del buffer (si tiene datos)
    if (paquete->buffer->size > 0) {
        if (paquete->buffer->stream == NULL) {
            if (logger) log_error(logger, "Error: stream es NULL pero size > 0");
            return;
        }
        
        bytes_enviados = send(socket_cliente, paquete->buffer->stream, paquete->buffer->size, 0);
        if (bytes_enviados != paquete->buffer->size) {
            if (logger) log_error(logger, "Error al enviar contenido del buffer. Enviados: %zd, esperados: %d", bytes_enviados, paquete->buffer->size);
            return;
        }
    }
    
    if (logger) {
        log_info(logger, "Paquete enviado exitosamente - Cod OP: %d, Tamaño: %d bytes", paquete->codigo_operacion, paquete->buffer->size);
    }
}


void eliminar_paquete(t_paquete *paquete)
{
    if (!paquete) return;
    if (paquete->buffer) {
        free(paquete->buffer->stream);
        free(paquete->buffer);
    }
    free(paquete);
}
