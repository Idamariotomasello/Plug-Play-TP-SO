#ifndef MEMORY_S_H_
#define MEMORY_S_H_

#include <conexion.h>

typedef struct { int puerto; } t_srv_arg;

bool ms_leer(int32_t dir_fisica, int32_t tamanio, void *dest);
bool ms_escribir(int32_t dir_fisica, int32_t tamanio, void *src);
void *ms_atender_cpu(void *varg);
void ms_iniciar_servidor_cpus(int puerto);
void *hilo_servidor_cpus(void *varg);


#endif
