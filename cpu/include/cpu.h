#ifndef CPU_H_
#define CPU_H_

#include <conexion.h>


 
/* =========================================================
 * Contexto de ejecución — lo que viaja entre KS/KM y CPU
 * ========================================================= */
typedef struct {
    int32_t    pid;
    t_registros regs;
} t_contexto;
 

typedef enum {
    SYSCALL_NINGUNA = 0,
    SYSCALL_SLEEP,
    SYSCALL_STDIN,
    SYSCALL_STDOUT,
    SYSCALL_MUTEX_CREATE,
    SYSCALL_MUTEX_LOCK,
    SYSCALL_MUTEX_UNLOCK,
    SYSCALL_MEM_ALLOC,
    SYSCALL_MEM_FREE,
    SYSCALL_INIT_PROC
} e_tipo_syscall;

typedef struct {
    e_tipo_syscall tipo;
    char nombre[32];
    char parametro_1[64];
    char parametro_2[64];
    uint32_t valor_1;
    uint32_t valor_2;
} t_syscall_pendiente;
 

typedef struct {
    bool    activa;
    int32_t motivo;
} t_interrupcion;

typedef enum {
    CANAL_CPU_DISPATCH = 1,
    CANAL_CPU_INTERRUPT = 2
} e_canal_cpu_kernel_scheduler;
 
/* Globales del módulo */
t_log    *logger;
t_config *config;
int32_t   g_id_cpu;
 
/* Conexiones */
int fd_kernel_scheduler_dispatch;
int fd_kernel_scheduler_interrupt;
int fd_kernel_memory;
int fd_memory_stick;
 
/* Interrupción compartida entre hilos */
t_interrupcion interrupcion_pendiente;
pthread_mutex_t mutex_interrupcion_pendiente;
 
uint32_t cpu_leer_registro(t_registros *r, const char *n);
void cpu_escribir_registro(t_registros *r, const char *n, uint32_t v);
char *cpu_fetch(int32_t pid, uint32_t pc);
void cpu_mmu_traducir(uint32_t dir_logica, int32_t *num_seg, int32_t *despl);
bool cpu_leer_memoria(int32_t pid, uint32_t dir_logica, int32_t tamanio, void *dest);
bool cpu_escribir_memoria(int32_t pid, uint32_t dir_logica, int32_t tamanio, void *src);
void cpu_limpiar_syscall(t_syscall_pendiente *syscall);
bool cpu_execute(t_contexto *ctx, const char *linea, e_motivo_retorno *motivo, t_syscall_pendiente *syscall);
bool cpu_conectar_kernel_scheduler(const char *ip, int puerto);
bool cpu_enviar_retorno_kernel_scheduler(int32_t pid, e_motivo_retorno motivo, const t_syscall_pendiente *syscall);
void *hilo_interrupciones(void *arg);
void cpu_ciclo_instruccion(t_contexto *ctx);

 


#endif
