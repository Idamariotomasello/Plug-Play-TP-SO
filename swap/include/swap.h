#ifndef SWAP_H_
#define SWAP_H_

#include <conexion.h>

#define SWAP_MAX_BLOQUES 4096

/* Mapa de bloques
 * El módulo SWAP NO administra qué proceso tiene qué bloque;
 * eso es responsabilidad del Kernel Memory.
 * SWAP sólo expone: leer bloque N, escribir bloque N.
 */

extern t_log    *logger;
extern t_config *config;
 
extern int       fd_km;             /* socket hacia Kernel Memory          */
extern FILE     *g_swap_file;       /* archivo de swap                     */
extern int32_t   g_block_size;      /* BLOCK_SIZE del config               */
extern int32_t   g_swap_size;       /* SWAP_FILE_SIZE del config           */

bool  swap_escribir_bloque(int32_t numero_bloque, void *datos);
void *swap_leer_bloque(int32_t numero_bloque);
bool  swap_crear_archivo(const char *path, int32_t tamanio);
void  swap_atender_km(void);

#endif
