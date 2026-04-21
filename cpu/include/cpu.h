#ifndef CPU_H_
#define CPU_H_

#include <conexion.h>

/* =========================================================
 * Registros de la CPU (según enunciado)
 * ========================================================= */
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
 
/* =========================================================
 * Contexto de ejecución — lo que viaja entre KS/KM y CPU
 * ========================================================= */
typedef struct {
    int32_t    pid;
    t_registros regs;
} t_contexto;
 
/* =========================================================
 * Motivos de devolución del proceso al KS
 * ========================================================= */
typedef enum {
    MOTIVO_EXIT        = 0,
    MOTIVO_SYSCALL     = 1,
    MOTIVO_INTERRUPCION = 2,
    MOTIVO_SEG_FAULT   = 3,
} e_motivo_retorno;
 

typedef struct {
    bool    activa;
    int32_t motivo;
} t_interrupcion;
 
/* Globales del módulo */
t_log    *logger;
t_config *config;
int32_t   g_id_cpu;
 
/* Conexiones */
int fd_ks;
int fd_km;
int fd_ms;
 
/* Interrupción compartida entre hilos */
t_interrupcion g_irq;
pthread_mutex_t mutex_irq;
 
uint32_t cpu_leer_registro(t_registros *r, const char *n);
void cpu_escribir_registro(t_registros *r, const char *n, uint32_t v);
char *cpu_fetch(int32_t pid, uint32_t pc);
void cpu_mmu_traducir(uint32_t dir_logica, int32_t *num_seg, int32_t *despl);
bool cpu_leer_memoria(int32_t pid, uint32_t dir_logica, int32_t tamanio, void *dest);
bool cpu_escribir_memoria(int32_t pid, uint32_t dir_logica, int32_t tamanio, void *src);
bool cpu_execute(t_contexto *ctx, const char *linea,e_motivo_retorno *motivo);
void *hilo_interrupciones(void *arg);
void cpu_ciclo_instruccion(t_contexto *ctx);

 


#endif
