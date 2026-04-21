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
#include <semaphore.h>
#include <math.h>

/**
* @brief Imprime un saludo por consola
* @param quien Módulo desde donde se llama a la función
* @return No devuelve nada
*/
void saludar(char* quien);



extern t_log *logger;
extern t_config *config;

extern pthread_mutex_t mutex_io;
extern pthread_mutex_t mutex_procesos;

extern sem_t g_sem_listo;



typedef struct {
    t_log *logger;
    int fd_cliente;
} t_hilo_arg;


typedef enum
{    
    //Identificadores de módulos
    MENSAJE = 10,
    PAQUETE = 11,
    TIPO_KS = 12,
    TIPO_CPU = 13,
    TIPO_MS = 14,
    TIPO_SWAP = 15,
    TIPO_IO = 16,
    TIPO_KM = 17,
    OP_IO_SLEEP = 18,
    OP_IO_STDIN = 29,
    OP_IO_STDOUT = 20,
    

    //Handshakes
    HANDSHAKE_KS = 100,
    HANDSHAKE_CPU = 101,
    HANDSHAKE_MS = 102,
    HANDSHAKE_SWAP = 103,

    //Operaciones KS->KM
    OP_CREAR_PROCESO = 200,
    OP_FINALIZAR_PROCESO = 201,
    OP_SUSPENDER_PROCESO = 202,
    OP_DESSUSPENDER_PROCESO = 203,
    OP_LEER_DATOS = 204,
    OP_ESCRIBIR_DATOS = 205,
    OP_CONFIRMAR_DESALOJO = 206,


    //Operaciones CPU->KM
    OP_FETCH_INSTRUCCION = 300,
    OP_GET_CONTEXTO = 301,
    OP_SET_CONTEXTO = 302,
    OP_CREAR_SEGMENTO = 303,
    OP_ELIMINAR_SEGMENTO = 304,
    
    //Operaciones KM->KS
    OP_NUEVA_MEMORIA = 400,
    OP_MEMORIA_CORRUPTA = 401,
    OP_INICIAR_COMPACT = 402,
    OP_FIN_COMPACT = 403,
    
    //Operaciones KM->SWAP
    OP_SWAP_ESCRIBIR_BLOQUE = 500,
    OP_SWAP_LEER_BLOQUE = 501,

    //Operaciones MS->KM
    OP_LEER_MS = 600,
    OP_ESCRIBIR_MS = 601,

    //Operaciones KS->IO
    IO_SUBTIPO_SLEEP = 710,
    IO_SUBTIPO_STDIN = 711,
    IO_SUBTIPO_STDOUT = 712,

    //Respuestas
    OP_OK = 900,
    OP_ERROR = 901,

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


void inicializar_semaforos();
void destruir_semaforos();

#endif