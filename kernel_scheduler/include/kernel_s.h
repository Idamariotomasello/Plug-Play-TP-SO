#ifndef KERNEL_S_H_
#define KERNEL_S_H_

#include <conexion.h>

#define KS_MAX_PROCESOS 64

#define KS_MOTIVO_EXIT         0
#define KS_MOTIVO_SYSCALL      1
#define KS_MOTIVO_INTERRUPCION 2
#define KS_MOTIVO_SEG_FAULT    3

t_log *logger;
t_config *config;


// Variables globales
int fd_kernel_memory;


void ks_registrar_io(int fd, int32_t subtipo);
int ks_fd_io(int32_t subtipo);
const char *ks_nombre_io(int32_t subtipo);
bool ks_despachar_io_sleep(int32_t pid, int32_t tiempo_ms);
bool ks_despachar_io_stdin(int32_t pid, int32_t cantidad);
bool ks_despachar_io_stdout(int32_t pid, void *datos, int32_t
tamanio);
void ks_syscall_io(int32_t subtipo, int32_t pid, void *param, int32_t param_size);

void *atender_cliente_ks(void *varg);

void iniciar_servidor_puerto_escucha(void);

#endif // KERNEL_SCHEDULER_H
