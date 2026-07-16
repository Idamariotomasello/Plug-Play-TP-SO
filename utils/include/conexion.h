#ifndef CONEXION_H_
#define CONEXION_H_

#include <ifaddrs.h>
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
#include <limits.h>

/**
* @brief Imprime un saludo por consola
* @param quien Módulo desde donde se llama a la función
* @return No devuelve nada
*/
void saludar(char* quien);

#define MAX_SEGMENTOS   64
#define MAX_MS          16   /* máximo de Memory Sticks simultáneos */
#define KS_MAX_MUTEX 32
#define MAX_BLOQUES_SWAP_POR_TROZO 1024
#define MAX_TROZOS_POR_SEGMENTO 8

extern t_log *logger;
extern t_config *config;

extern pthread_mutex_t mutex_io;
extern pthread_mutex_t mutex_procesos;
extern pthread_mutex_t mutex_irq;
extern pthread_mutex_t mutex_km;
extern pthread_mutex_t  g_mutex_cola_ready;
extern pthread_mutex_t mutex_fd_ks;
extern pthread_mutex_t mutex_ms_lista;
extern pthread_mutex_t mutex_interrupcion_pendiente;
extern pthread_mutex_t mutex_km_socket;
extern pthread_mutex_t mutex_huecos;
extern pthread_mutex_t mutex_fd_ks;
extern pthread_mutex_t mutex_fd_swap;
extern pthread_mutex_t mutex_ms_ops[MAX_MS];
extern sem_t g_sem_listo;


/* Semáforos de sincronización inter-módulo */
extern sem_t sem_km_cpu;   /* KM→CPU:  KM post luego de responder contexto/fetch  */
extern sem_t sem_cpu_ks;   /* CPU→KS:  CPU post antes de enviar syscall al KS     */
extern sem_t sem_ks_km;    /* KS→KM:   KS post antes de enviar OP_CREAR_SEGMENTO  */
extern sem_t sem_km_ks;    /* KM→KS:   KM post luego de confirmar segmento creado */

typedef struct {
    t_log *logger;
    int fd_cliente;
} t_hilo_arg;

typedef enum {
    CANAL_CPU_DISPATCH = 1,
    CANAL_CPU_INTERRUPT = 2
} e_canal_cpu_kernel_scheduler;



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
    IO_TIPO_INVALIDO = 0,
    

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
    OP_GET_MS_LIST = 305,
    OP_GET_SEGMENT_MAX_SIZE = 306,
    
    //Operaciones KM->KS
    OP_NUEVA_MEMORIA = 500,
    OP_MEMORIA_CORRUPTA = 501,
    OP_INICIAR_COMPACT = 502,
    OP_FIN_COMPACT = 503,
    
    //Operaciones KM->SWAP
    OP_SWAP_ESCRIBIR_BLOQUE = 600,
    OP_SWAP_LEER_BLOQUE = 601,

    //Operaciones MS->KM
    OP_LEER_MS = 700,
    OP_ESCRIBIR_MS = 701,

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


/* Registros de la CPU */

typedef struct {
    uint32_t PC;   /* Program Counter — índice de instrucción */
    uint8_t  AX;
    uint8_t  BX;
    uint8_t  CX;
    uint8_t  DX;
    uint32_t EAX;
    uint32_t EBX;
    uint32_t ECX;
    uint32_t EDX;
    uint32_t SI;   /* dirección lógica origen  */
    uint32_t DI;   /* dirección lógica destino */
} t_registros;

/* Info: Estados del proceso (7 estados). */
typedef enum {
    ESTADO_NEW = 0,
    ESTADO_READY = 1,
    ESTADO_EXEC = 2,
    ESTADO_BLOCK = 3,
    ESTADO_SUSP_BLOCK = 4,
    ESTADO_SUSP_READY = 5,
    ESTADO_EXIT = 6
} e_estado_proceso;

/* Info: Motivos de retorno del proceso. */
typedef enum {
    MOTIVO_NINGUNO = 0,
    MOTIVO_SYSCALL = 1,
    MOTIVO_INTERRUPCION = 2,
    MOTIVO_ERROR = 3,
    MOTIVO_EXIT = 4,
    MOTIVO_SEG_FAULT = 5
} e_motivo_retorno;


typedef struct {
    bool     activo;
    int32_t  ms_id;          // >=0: Memory Stick; -1: Swap
    int32_t  dir_fisica_ms;  // offset dentro del MS o número de bloque swap
    int32_t  offset_seg;     // offset dentro del segmento lógico
    int32_t  tamanio;        // bytes del trozo
    bool     en_swap;        // true si el trozo está en swap
    int32_t num_bloques_swap; // cantidad de bloques swap utilizados
    int32_t bloques_swap[MAX_BLOQUES_SWAP_POR_TROZO];
} t_trozo_segmento;

typedef struct {
    bool    activo;
    int32_t seg_id;
    int32_t base;            /* base lógica del segmento (= seg_id * SEGMENT_MAX_SIZE) */
    int32_t limite;          /* tamaño total del segmento en bytes  */
    int32_t n_trozos;
    t_trozo_segmento trozos[MAX_MS]; /* cómo está distribuido entre MSs */
} t_segmento;

/* Contexto extendido con tabla de segmentos */
typedef struct {
    int32_t     pid;
    t_registros regs;
    int32_t     n_segmentos;
    t_segmento  segmentos[MAX_SEGMENTOS];
} t_contexto;

/* Info de un MS conocido por la CPU */
typedef struct {
    int     fd;
    int32_t tamanio;
    int32_t base_global;  /* offset global: suma de tamaños de MSs anteriores */
} t_ms_info;

typedef struct s_pcb {
    bool activo;                 // para marcar PCB como eliminado
    int32_t pid;
    int32_t prioridad;
    int32_t prioridad_original;   // para herencia
    e_estado_proceso estado;
    t_registros registros;         // copia de los registros
    uint32_t tiempo_bloqueo;       // para timeout de suspension
    char *mutex_esperado;          // si está bloqueado por mutex
    struct s_pcb *siguiente;
    time_t tiempo_suspension;      // timestamp cuando se suspendió
    char syscall_nombre[64];       // para syscalls, nombre de la instrucción que la disparó
    char syscall_arg1[64];        // para syscalls, argumento 1 (si tiene)
    char syscall_arg2[64];        // para syscalls, argumento 2 (si
    int     fd_cpu_asignada;   // fd de la CPU que lo ejecuta actualmente
    uint32_t syscall_val1;
    uint32_t syscall_val2;
    struct timespec tiempo_block_inicio;
    bool suspension_en_curso; 
    bool dessuspender_al_terminar; // si true, al terminar de ejecutar se pasa a SUSP_READY
    bool     stdin_pendiente;
    uint32_t stdin_dir_logica;
    int32_t  stdin_tamanio;
    void    *stdin_buffer;
} t_pcb;

typedef struct s_proceso_esperando {
    t_pcb                    *pcb;
    struct s_proceso_esperando *siguiente;
} t_proceso_esperando;

typedef struct {
    char                 nombre[64];
    bool                 tomado;
    int32_t              pid_duenio;       // PID que lo tiene tomado (-1 si libre)
    int32_t              prioridad_original_duenio; // para restaurar tras herencia
    t_proceso_esperando *cola_espera;      // procesos bloqueados esperando este mutex
    bool                 activo;
} t_mutex;


typedef struct
{
	op_code codigo_operacion;
	t_buffer* buffer;
} t_paquete;


typedef struct s_nodo_ready {
    t_pcb              *pcb;
    struct s_nodo_ready *siguiente;
} t_nodo_ready;


extern t_nodo_ready    *g_cola_ready_head;
extern t_nodo_ready    *g_cola_ready_tail;
extern int           g_ms_count;
extern int32_t       g_swap_bloques_totales;

void inicializar_semaforos();
void destruir_semaforos();

#endif
