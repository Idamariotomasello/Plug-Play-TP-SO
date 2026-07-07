#ifndef KERNEL_MEMORY_H
#define KERNEL_MEMORY_H

#include "conexion.h"

#define KM_MAX_PROCESOS     64
#define KM_MAX_INSTRUCCIONES 1024
#define KM_MAX_HUECOS   256
#define KM_MAX_SWAP_BLOQUES 1024

#define MAX_HUECOS           (MAX_MS * 128)   /* huecos máximos en toda la memoria */
#define MAX_SWAP_BLOQUES     4096             /* bloques máximos en swap            */

#define TROZO_HAS_EN_SWAP 1


/* Globales */

extern bool          g_procesos_init;
extern char          g_scripts_basepath[1024];

extern int32_t       g_memoria_total;
extern int           g_huecos_count;
 
/* Mapa de bloques swap: true = libre */
extern bool          g_swap_bloques_libres[MAX_SWAP_BLOQUES];
extern int           fd_swap_global;           /* socket hacia SWAP    */

extern int g_cpus_conectadas;
extern int g_ms_conectados;
extern int g_swap_conectado;
extern int fd_ks_global;


/* Estrategias de asignación */
 
typedef enum {
    KM_STRATEGY_FIRST_FIT = 0,
    KM_STRATEGY_BEST_FIT  = 1,
    KM_STRATEGY_WORST_FIT = 2
} e_km_strategy;


/* Proceso en Kernel Memory */
 
typedef struct {
    bool         activo;
    int32_t      pid;
    char       **instrucciones;
    int32_t      n_instrucciones;
    t_registros  registros;
    t_segmento   segmentos[MAX_SEGMENTOS];
    int32_t      n_segmentos;
} t_proceso_km;


/* Memory Stick conocido por KM */
 
typedef struct {
    int     fd;
    int32_t tamanio;
    int32_t base_global;     /* suma de tamaños de MSs anteriores      */
    bool    activo;
    char    ip[64];
    int32_t puerto_cpus;
} t_ms_entry;


/* Lista de Memory Sticks conectados */
typedef struct {
    int     fd;
    int32_t tamanio;
    int32_t base_global;   /* suma de tamaños anteriores */
    bool    activo;
    char    ip[64];
    int32_t puerto_cpus;
} t_ms_km;


typedef struct
{
    t_log   *logger;
    int      fd_cliente;
    int32_t  tipo;
} t_km_hilo_arg;


/* Hueco de memoria libre
 * t_hueco representa un bloque contiguo libre dentro de un MS. 
 */
 
typedef struct {
    bool    activo;          /* true = hueco válido                     */
    int32_t ms_id;           /* índice en g_ms_lista                    */
    int32_t base;            /* dirección física dentro del MS          */
    int32_t tamanio;         /* bytes disponibles                       */
} t_hueco;



/* Prototipos — instrucciones y contexto */
void  km_init_procesos(void);
bool  km_cargar_instrucciones(int32_t pid, const char *path);
char *km_obtener_instruccion(int32_t pid, int32_t pc);
int   km_contar_instrucciones(const char *path);
void *km_leer_campo(void *stream, int stream_size, int *offset, int *campo_size);
bool  km_recibir_cuerpo_paquete(int fd, void **stream_out, int *size_out);

/* Prototipos — gestión de procesos (KS) */
void km_procesar_crear_proceso(int fd_ks, void *stream, int stream_size);
void km_finalizar_proceso(int fd_ks, int32_t pid);
void km_suspender_proceso(int fd_ks, int32_t pid);
void km_dessuspender_proceso(int fd_ks, int32_t pid);

/* Prototipos — segmentos */
void km_crear_segmento(int fd_ks, int32_t pid, int32_t seg_id, int32_t tam_seg);
void km_eliminar_segmento(int fd_ks, int32_t pid, int32_t seg_id);
bool km_asignar_segmento(int32_t pid, int32_t seg_id, int32_t tam_seg);
void km_guardar_segmento(int32_t pid, const t_segmento *seg);
 
/* Prototipos — huecos */
t_hueco *km_buscar_hueco(int32_t tamanio);
void     km_liberar_trozo(const t_trozo_segmento *trozo);
void     km_hueco_agregar(const t_hueco *h);
void     km_mergear_huecos(int32_t ms_id);
bool km_traducir_global_a_ms(int32_t dir_global, int32_t *ms_id_out, int32_t *offset_out);
 
/* Prototipos — SWAP */
int32_t km_swap_reservar_bloque(void);
void    km_swap_liberar_bloque(int32_t bloque);
bool    km_swap_escribir(int32_t bloque, void *datos, int32_t tamanio);
bool    km_swap_leer(int32_t bloque, void *dest, int32_t tamanio);
 
/* Prototipos — lectura/escritura de datos (KS intermediario) */
void km_leer_datos(int fd_ks, int32_t pid, int32_t dir_logica, int32_t tamanio);
void km_escribir_datos(int fd_ks, int32_t pid, int32_t dir_logica, int32_t tamanio, void *datos);
 
/* Prototipos — servidores y despachadores */
void  km_iniciar_servidor(t_log *logger, int puerto);
void *km_despachar_cliente(void *varg);
void  km_atender_ks(t_log *logger, int fd_ks);
void  km_atender_cpu(t_log *logger, int fd_cpu);
void  km_atender_ms(t_log *logger, int fd_ms);
void  km_atender_swap(t_log *logger, int fd_swap);
void *hilo_servidor(void *varg);
 
/* Helpers internos de sincronización */
void inicializar_semaforos(void);
void destruir_semaforos(void);


#endif /* KERNEL_MEMORY_H */