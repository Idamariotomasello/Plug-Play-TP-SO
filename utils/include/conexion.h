#ifndef CONEXION_H_
#define CONEXION_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <commons/log.h>
#include <commons/config.h>

/**
* @brief Imprime un saludo por consola
* @param quien Módulo desde donde se llama a la función
* @return No devuelve nada
*/
void saludar(char* quien);



extern t_log *logger;
extern t_config *config;

typedef struct {
    t_log *logger;
    int fd_cliente;
} t_hilo_arg;


typedef enum
{
    //Handshakes
    GET_BLOCK_SIZE = 100,
    BLOCK_SIZE = 101,
    HANDSHAKE_WORKER = 102,
    CONFIRMATION = 103,
    HANDSHAKE_QUERY_CONTROL = 105,

    //Operaciones Archivos
    OP_CREATE = 200,
    OP_READ = 201,
    OP_WRITE = 202,
    OP_TRUNCATE = 203,
    OP_DELETE = 204,
    OP_TAG = 205,
    OP_COMMIT = 206,
    OP_FLUSH = 207,
    OP_END = 208,

    //Respuestas
    OP_OK = 400,
    OP_ERROR = 401,

    RESULTADO_READ = 300,
    OP_PROGRAM_COUNTER = 301,
    MENSAJE_LECTURA = 302,
    ERROR_EJECUCION = 303,
    
    //Identificadores de módulos
    MENSAJE = 10,
    PAQUETE = 11,
    TIPO_KS = 12,
    TIPO_CPU = 13,
    TIPO_MS = 14,
    TIPO_SWAP = 15,
    TIPO_IO = 16,
    TIPO_KM = 17,
    

    EXIT,
    SYSCALL_ERROR,
    SYSCALL_OK
}op_code;

typedef struct
{
	int size;
	void* stream;
} t_buffer;


typedef struct
{
	op_code codigo_operacion;
	t_buffer* buffer;
} t_paquete;


#endif