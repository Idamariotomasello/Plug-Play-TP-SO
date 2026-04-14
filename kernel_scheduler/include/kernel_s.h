#ifndef KERNEL_S_H_
#define KERNEL_S_H_

#include <conexion.h>

t_log *logger;
t_config *config;


// Variables globales
int fd_kernel_memory = -1;

// Prototipos de funciones
void *atender_cpu(void *varg);
void *atender_io(void *varg);
void iniciar_servidor_puerto_escucha(void);

#endif // KERNEL_SCHEDULER_H
