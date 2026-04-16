/*
 * kernel_memory.h
 * Declaraciones específicas del módulo Kernel Memory.
 */

#ifndef KERNEL_MEMORY_H
#define KERNEL_MEMORY_H

#include <pthread.h>
#include <commons/log.h>
#include "server.h"

/* =========================================================
 * Argumento empaquetado para el hilo despachador
 * ========================================================= */

typedef struct
{
    t_log   *logger;
    int      fd_cliente;
    int32_t  tipo;
} t_km_hilo_arg;

/* =========================================================
 * API del servidor KM
 * ========================================================= */

void  km_iniciar_servidor(t_log *logger, int puerto);
void *km_despachar_cliente(void *arg);
void  km_atender_ks(t_log *logger, int fd_ks);
void  km_atender_cpu(t_log *logger, int fd_cpu);
void  km_atender_ms(t_log *logger, int fd_ms);
void  km_atender_swap(t_log *logger, int fd_swap);

#endif /* KERNEL_MEMORY_H */