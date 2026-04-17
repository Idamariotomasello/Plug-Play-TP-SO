#ifndef SWAP_H_
#define SWAP_H_

#include <conexion.h>

bool swap_escribir_bloque(int32_t numero_bloque, void *datos);
void *swap_leer_bloque(int32_t numero_bloque);
void swap_atender_km(void);
bool swap_crear_archivo(const char *path, int32_t tamanio);

#endif
