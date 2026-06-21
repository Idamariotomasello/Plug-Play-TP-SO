#ifndef KERNEL_S_H_
#define KERNEL_S_H_

#include <conexion.h>

#define KS_MAX_PROCESOS 64
#define MAX_CPUS 8

#define KS_MOTIVO_EXIT         4
#define KS_MOTIVO_SYSCALL      1
#define KS_MOTIVO_INTERRUPCION 2
#define KS_MOTIVO_SEG_FAULT    5
#define KS_MOTIVO_NINGUNO      0

t_log *logger;
t_config *config;

typedef enum { ALGO_FIFO, ALGO_RR, ALGO_CMN } e_algoritmo;

e_algoritmo g_algoritmo;
int32_t     g_quantum_ms;
bool        g_preemption;
int32_t     g_suspension_ms;

typedef struct {
    int         fd_cpu;        // fd de la CPU que lo ejecuta (-1 si libre)
    pthread_t   hilo_quantum;  // hilo del timer RR
    bool        quantum_activo;
} t_exec_slot;

// Variables globales
int fd_kernel_memory;

t_pcb *ks_buscar_pcb(int32_t pid);
void   ks_cambiar_estado(t_pcb *pcb, e_estado_proceso nuevo);
void   ks_encolar_ready(t_pcb *pcb);
t_mutex *ks_buscar_mutex(const char *nombre);
void ks_registrar_io(int fd, int32_t subtipo);
void ks_encolar_ready(t_pcb *pcb);
t_pcb *cola_ready_desencolar(void);
void cola_ready_encolar_frente(t_pcb *pcb);
t_pcb *ks_primer_ready(void);
int ks_fd_io(int32_t subtipo);
const char *ks_nombre_io(int32_t subtipo);
bool ks_despachar_io_sleep(int32_t pid, int32_t tiempo_ms);
bool ks_despachar_io_stdin(int32_t pid, int32_t cantidad);
bool ks_despachar_io_stdout(int32_t pid, void *datos, int32_t
tamanio);
void ks_syscall_io(int32_t subtipo, int32_t pid, void *param, int32_t param_size);
bool ks_escribir_datos(int32_t pid, int32_t dir_logica, int32_t tamanio, void *datos);

void *atender_cliente_ks(void *varg);

void iniciar_servidor_puerto_escucha(void);

#endif // KERNEL_SCHEDULER_H
