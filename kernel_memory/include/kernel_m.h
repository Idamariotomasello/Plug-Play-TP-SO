#ifndef KERNEL_MEMORY_H
#define KERNEL_MEMORY_H

#include "conexion.h"

#define KM_MAX_PROCESOS     64
#define KM_MAX_INSTRUCCIONES 1024
 
typedef struct {
    bool     activo;
    int32_t  pid;
    char   **instrucciones;
    int      n_instrucciones;
    t_registros registros;
    int32_t     n_segmentos;
    t_segmento  segmentos[MAX_SEGMENTOS];
} t_proceso_km;

/* Lista de Memory Sticks conectados */
typedef struct {
    int     fd;
    int32_t tamanio;
    int32_t base_global;   /* suma de tamaños anteriores */
    bool    activo;
} t_ms_km;

typedef struct
{
    t_log   *logger;
    int      fd_cliente;
    int32_t  tipo;
} t_km_hilo_arg;

t_ms_km  g_ms_lista[MAX_MS];
int      g_ms_count      = 0;
int32_t  g_memoria_total = 0;
int fd_ks_global = -1;   /* para notificar al KS */


void km_init_procesos(void);
bool km_cargar_instrucciones(int32_t pid, const char *path);
char *km_obtener_instruccion(int32_t pid, int32_t pc);
void *km_leer_campo(void *stream, int stream_size, int *offset, int *campo_size);
bool km_recibir_cuerpo_paquete(int fd, void **stream_out, int *size_out);
int km_contar_instrucciones(const char *path);
void km_procesar_crear_proceso(int fd_ks, void *stream, int stream_size);

void  km_iniciar_servidor(t_log *logger, int puerto);
void *km_despachar_cliente(void *arg);
void  km_atender_ks(t_log *logger, int fd_ks);
void  km_atender_cpu(t_log *logger, int fd_cpu);
void  km_atender_ms(t_log *logger, int fd_ms);
void  km_atender_swap(t_log *logger, int fd_swap);

#endif /* KERNEL_MEMORY_H */