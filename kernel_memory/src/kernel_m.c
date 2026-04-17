#include "kernel_m.h"
#include <cliente.h>
#include <server.h>

/*
int main(int argc, char* argv[]) {
    saludar("kernel_memory");
    return 0;
}
*/

t_config *g_config  = NULL;
t_log    *g_logger  = NULL;

int g_cpus_conectadas = 0;
int g_ms_conectados   = 0;
int g_swap_conectado  = 0;

/* =========================================================
 * km_leer_campo
 * Extrae un campo del stream con formato:
 *   tamanio (int) | datos (tamanio bytes)
 * Avanza *offset. Devuelve malloc'd o NULL si falla.
 * ========================================================= */
 
static void *km_leer_campo(void *stream, int stream_size,
                            int *offset, int *campo_size)
{
    if (*offset + (int)sizeof(int) > stream_size)
        return NULL;
 
    memcpy(campo_size, (char *)stream + *offset, sizeof(int));
    *offset += sizeof(int);
 
    if (*campo_size <= 0 || *offset + *campo_size > stream_size)
        return NULL;
 
    void *dato = malloc(*campo_size);
    if (!dato) return NULL;
 
    memcpy(dato, (char *)stream + *offset, *campo_size);
    *offset += *campo_size;
    return dato;
}
 
/* =========================================================
 * km_recibir_cuerpo_paquete
 * Lee size(int) + stream(size bytes) después de que el
 * cod_op ya fue leído. Compatible con enviar_paquete().
 * ========================================================= */
 
static bool km_recibir_cuerpo_paquete(int fd, void **stream_out, int *size_out)
{
    if (recv(fd, size_out, sizeof(int), MSG_WAITALL) != sizeof(int))
        return false;
 
    if (*size_out <= 0) { *stream_out = NULL; return true; }
 
    *stream_out = malloc(*size_out);
    if (!*stream_out) return false;
 
    if (recv(fd, *stream_out, *size_out, MSG_WAITALL) != *size_out) {
        free(*stream_out);
        return false;
    }
    return true;
}
 
/* =========================================================
 * km_contar_instrucciones
 * Abre el archivo de script y cuenta líneas no vacías.
 * ========================================================= */
 
static int km_contar_instrucciones(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
 
    int count = 0;
    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        /* ignorar líneas vacías o solo con \n */
        int len = strlen(linea);
        int solo_blancos = 1;
        for (int i = 0; i < len; i++) {
            if (linea[i] != ' ' && linea[i] != '\t' &&
                linea[i] != '\n' && linea[i] != '\r') {
                solo_blancos = 0;
                break;
            }
        }
        if (!solo_blancos) count++;
    }
    fclose(f);
    return count;
}


/* =========================================================
 * km_procesar_crear_proceso
 * Deserializa OP_CREAR_PROCESO, arma el path, cuenta
 * instrucciones y loguea todo.
 * Campos en el stream (formato agregar_a_paquete):
 *   [0] PID        — int32_t
 *   [1] prioridad  — int32_t
 *   [2] nombre     — char[] sin '\0'
 * ========================================================= */
 
static void km_procesar_crear_proceso(int fd_ks,
                                       void *stream, int stream_size)
{
    int offset = 0, campo_size;
 
    /* PID */
    void *raw = km_leer_campo(stream, stream_size, &offset, &campo_size);
    if (!raw) { log_error(g_logger, "CREATE_PROCESS: error leyendo PID"); return; }
    int32_t pid; memcpy(&pid, raw, sizeof(int32_t)); free(raw);
 
    /* Prioridad */
    raw = km_leer_campo(stream, stream_size, &offset, &campo_size);
    if (!raw) { log_error(g_logger, "CREATE_PROCESS: error leyendo prioridad"); return; }
    int32_t prioridad; memcpy(&prioridad, raw, sizeof(int32_t)); free(raw);
 
    /* Nombre del script */
    raw = km_leer_campo(stream, stream_size, &offset, &campo_size);
    if (!raw) { log_error(g_logger, "CREATE_PROCESS: error leyendo nombre script"); return; }
    char *nombre = malloc(campo_size + 1);
    memcpy(nombre, raw, campo_size);
    nombre[campo_size] = '\0';
    free(raw);
 
    /* Armar path completo */
    char *base = config_get_string_value(g_config, "SCRIPTS_BASEPATH");
    char  path[1024];
    snprintf(path, sizeof(path), "%s/%s", base, nombre);
    free(base);
 
    /* Contar instrucciones abriendo el archivo */
    int n_instrucciones = km_contar_instrucciones(path);
 
    /* ── Logs obligatorios del enunciado ── */
    log_info(g_logger, "## PID: %d - Proceso Creado", pid);
    log_info(g_logger, "## PID: %d - Script: %s", pid, nombre);
    log_info(g_logger, "## PID: %d - Path completo: %s", pid, path);
    log_info(g_logger, "## PID: %d - Prioridad: %d", pid, prioridad);
 
    if (n_instrucciones >= 0)
        log_info(g_logger, "## PID: %d - Instrucciones: %d lineas", pid, n_instrucciones);
    else
        log_warning(g_logger, "## PID: %d - No se pudo abrir '%s' — "
                    "verificar SCRIPTS_BASEPATH en kernel_m.config", pid, path);
 
    free(nombre);
 
    /* Responder OK al KS */
    enviar_int32(fd_ks, OP_OK);
}
 
/* =========================================================
 * km_iniciar_servidor — accept loop principal
 * ========================================================= */
 
void km_iniciar_servidor(t_log *logger, int puerto)
{
    int fd_escucha = iniciar_servidor(logger, puerto);
    if (fd_escucha == -1) {
        log_error(logger, "No se pudo iniciar servidor KM");
        return;
    }
 
    log_info(logger, "Kernel Memory esperando conexiones en puerto %d", puerto);
 
    while (1) {
        int fd_cliente = esperar_cliente(logger, fd_escucha);
        if (fd_cliente == -1) continue;
 
        int32_t tipo = recibir_handshake(logger, fd_cliente);
        if (tipo == HANDSHAKE_ERR) continue;
 
        t_km_hilo_arg *arg = malloc(sizeof(t_km_hilo_arg));
        if (!arg) { close(fd_cliente); continue; }
 
        arg->logger     = logger;
        arg->fd_cliente = fd_cliente;
        arg->tipo       = tipo;
 
        pthread_t hilo;
        pthread_create(&hilo, NULL, km_despachar_cliente, arg);
        pthread_detach(hilo);
    }
 
    close(fd_escucha);
}
 
/* =========================================================
 * km_despachar_cliente
 * ========================================================= */
 
void *km_despachar_cliente(void *varg)
{
    t_km_hilo_arg *arg = (t_km_hilo_arg *)varg;
    t_log  *logger = arg->logger;
    int     fd     = arg->fd_cliente;
    int32_t tipo   = arg->tipo;
    free(arg);
 
    switch (tipo) {
        case TIPO_KS:   km_atender_ks(logger, fd);   break;
        case TIPO_CPU:  km_atender_cpu(logger, fd);  break;
        case TIPO_MS:   km_atender_ms(logger, fd);   break;
        case TIPO_SWAP: km_atender_swap(logger, fd); break;
        default:
            log_warning(logger, "Tipo desconocido: %d (fd=%d)", tipo, fd);
            close(fd);
            break;
    }
    return NULL;
}
 
/* =========================================================
 * km_atender_ks
 * ========================================================= */
 
void km_atender_ks(t_log *logger, int fd_ks)
{
    log_info(logger, "## Kernel Scheduler Conectado - FD del socket: %d", fd_ks);
 
    int32_t cod_op;
    while (recibir_int32(fd_ks, &cod_op)) {
 
        void *stream  = NULL;
        int   sz      = 0;
 
        if (!km_recibir_cuerpo_paquete(fd_ks, &stream, &sz)) {
            log_error(logger, "KS — error recibiendo cuerpo (cod_op=%d)", cod_op);
            break;
        }
 
        switch (cod_op) {
 
            case OP_CREAR_PROCESO:
                km_procesar_crear_proceso(fd_ks, stream, sz);
 
                /* ── Esperar al menos 1 CPU + 1 MS + 1 SWAP ── */
                log_info(logger, "Esperando modulos: CPU, Memory Stick y SWAP...");
                sem_wait(&g_sem_listo);   /* 1ª señal: primera CPU    */
                sem_wait(&g_sem_listo);   /* 2ª señal: primer MS      */
                sem_wait(&g_sem_listo);   /* 3ª señal: SWAP           */
 
                log_info(logger, "Sistema listo — CPUs: %d | MS: %d | SWAP: %d",
                         g_cpus_conectadas, g_ms_conectados, g_swap_conectado);
                log_info(logger, "## PID: 0 listo para ejecutarse (prioridad maxima)");
                break;
 
            default:
                log_info(logger, "KS — cod_op recibido: %d", cod_op);
                break;
        }
 
        if (stream) free(stream);
    }
 
    log_warning(logger, "Kernel Scheduler desconectado (fd=%d)", fd_ks);
    close(fd_ks);
}
 
/* =========================================================
 * km_atender_cpu
 * ========================================================= */
 
void km_atender_cpu(t_log *logger, int fd_cpu)
{
    int32_t id_cpu;
    if (!recibir_int32(fd_cpu, &id_cpu)) {
        log_error(logger, "Error recibiendo ID de CPU (fd=%d)", fd_cpu);
        close(fd_cpu);
        return;
    }
 
    __atomic_add_fetch(&g_cpus_conectadas, 1, __ATOMIC_SEQ_CST);
    log_info(logger, "## CPU %d Conectada (fd=%d) — total: %d",
             id_cpu, fd_cpu, g_cpus_conectadas);
 
    if (g_cpus_conectadas == 1)
        sem_post(&g_sem_listo);
 
    int32_t cod_op;
    while (recibir_int32(fd_cpu, &cod_op))
        log_info(logger, "CPU %d — cod_op: %d", id_cpu, cod_op);
 
    __atomic_sub_fetch(&g_cpus_conectadas, 1, __ATOMIC_SEQ_CST);
    log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd_cpu);
    close(fd_cpu);
}


/* =========================================================
 * km_atender_ms
 * ========================================================= */
 
void km_atender_ms(t_log *logger, int fd_ms)
{
    int32_t tamanio;
    if (!recibir_int32(fd_ms, &tamanio)) {
        log_error(logger, "Error recibiendo tamaño MS (fd=%d)", fd_ms);
        close(fd_ms);
        return;
    }
 
    __atomic_add_fetch(&g_ms_conectados, 1, __ATOMIC_SEQ_CST);
    log_info(logger, "## Memory Stick de %d bytes Conectada (fd=%d) — total: %d",
             tamanio, fd_ms, g_ms_conectados);
 
    if (g_ms_conectados == 1)
        sem_post(&g_sem_listo);
 
    char probe;
    if (recv(fd_ms, &probe, 1, MSG_WAITALL) <= 0)
        log_error(logger, "## Memory Stick (fd=%d) desconectado — memoria corrupta", fd_ms);
 
    __atomic_sub_fetch(&g_ms_conectados, 1, __ATOMIC_SEQ_CST);
    close(fd_ms);
}
 
/* =========================================================
 * km_atender_swap
 * ========================================================= */
 
void km_atender_swap(t_log *logger, int fd_swap)
{
    int32_t block_size, swap_size;
    if (!recibir_int32(fd_swap, &block_size) ||
        !recibir_int32(fd_swap, &swap_size))
    {
        log_error(logger, "Error recibiendo metadatos SWAP (fd=%d)", fd_swap);
        close(fd_swap);
        return;
    }
 
    int32_t bloques_totales = (block_size > 0) ? swap_size / block_size : 0;
 
    __atomic_add_fetch(&g_swap_conectado, 1, __ATOMIC_SEQ_CST);
    log_info(logger, "## Conectado a SWAP (fd=%d) — bloque: %d bytes, total: %d bytes, bloques disponibles: %d",
             fd_swap, block_size, swap_size, bloques_totales);
 
    if (g_swap_conectado == 1)
        sem_post(&g_sem_listo);
 
    /*
     * El SWAP es pasivo: el KM inicia todos los pedidos
     * via OP_SWAP_ESCRIBIR_BLOQUE / OP_SWAP_LEER_BLOQUE.
     * Este recv bloqueante detecta desconexion inesperada.
     */
    char probe;
    if (recv(fd_swap, &probe, 1, MSG_WAITALL) <= 0)
        log_error(logger, "SWAP (fd=%d) desconectado inesperadamente — "
                  "procesos suspendidos irrecuperables", fd_swap);
 
    __atomic_sub_fetch(&g_swap_conectado, 1, __ATOMIC_SEQ_CST);
    close(fd_swap);
}
 
/* =========================================================
 * Hilo del servidor
 * ========================================================= */

static void *hilo_servidor(void *varg)
{
    t_servidor_arg *arg = (t_servidor_arg *)varg;
    km_iniciar_servidor(arg->logger, arg->puerto);
    free(arg);
    return NULL;
}
 
/* =========================================================
 * main
 * ========================================================= */
 
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s [archivo_config]\n", argv[0]);
        return 1;
    }
 
    g_logger = log_create("kernel_memory.log", "KERNEL_MEMORY", true, LOG_LEVEL_INFO);
    if (!g_logger) { fprintf(stderr, "Error al crear logger\n"); return 1; }
 
    g_config = config_create(argv[1]);
    if (!g_config) {
        char alt[512];
        snprintf(alt, sizeof(alt), "../%s", argv[1]);
        g_config = config_create(alt);
    }
    if (!g_config) {
        log_error(g_logger, "No se pudo cargar '%s'", argv[1]);
        log_error(g_logger, "  cd kernel_memory && ./bin/kernel_memory kernel_m.config");
        log_destroy(g_logger);
        return 1;
    }
 
    /* El semáforo g_sem_listo está declarado en conexion.c
     * e inicializado con inicializar_semaforos() */
    inicializar_semaforos();
 
    int puerto = config_get_int_value(g_config, "PUERTO_ESCUCHA");
    log_info(g_logger, "Kernel Memory iniciando en puerto %d", puerto);
 
    t_servidor_arg *arg = malloc(sizeof(t_servidor_arg));
    arg->logger = g_logger;
    arg->puerto = puerto;
 
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, hilo_servidor, arg) != 0) {
        log_error(g_logger, "Error al crear hilo servidor: %s", strerror(errno));
        free(arg);
        destruir_semaforos();
        config_destroy(g_config);
        log_destroy(g_logger);
        return 1;
    }
 
    pthread_join(hilo, NULL);
 
    log_info(g_logger, "Kernel Memory finalizado");
    destruir_semaforos();
    config_destroy(g_config);
    log_destroy(g_logger);
    return 0;
}
