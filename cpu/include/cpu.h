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
