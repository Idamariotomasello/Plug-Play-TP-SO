#include "swap.h"
#include <cliente.h>
#include <server.h>

/* =========================================================
 * Globales del módulo
 * ========================================================= */
 
t_log    *logger = NULL;
t_config *config = NULL;
 
int fd_km = -1;             /* socket hacia Kernel Memory */
FILE *g_swap_file = NULL;   /* archivo de swap abierto */
int32_t g_block_size = 0;   /* BLOCK_SIZE del config */
int32_t g_swap_size = 0;    /* SWAP_FILE_SIZE del config */
int32_t  g_swap_bloques_totales = 0;
 
/* =========================================================
 * swap_escribir_bloque
 * Escribe exactamente g_block_size bytes en el bloque
 * indicado por numero_bloque (base 0).
 * ========================================================= */
 
bool swap_escribir_bloque(int32_t numero_bloque, void *datos)
{
    if (!g_swap_file) {
        log_error(logger, "Archivo de SWAP no abierto");
        return false;
    }
 
    long offset = (long)numero_bloque * g_block_size;
    if (offset + g_block_size > g_swap_size) {
        log_error(logger,
                  "Bloque %d fuera de rango (swap_size=%d, block_size=%d)",
                  numero_bloque, g_swap_size, g_block_size);
        return false;
    }
 
    if (fseek(g_swap_file, offset, SEEK_SET) != 0) {
        log_error(logger, "fseek fallo en bloque %d: %s",
                  numero_bloque, strerror(errno));
        return false;
    }
 
    size_t escritos = fwrite(datos, 1, g_block_size, g_swap_file);
    fflush(g_swap_file);
 
    if ((int32_t)escritos != g_block_size) {
        log_error(logger, "Error escribiendo bloque %d: %zu/%d bytes",
                  numero_bloque, escritos, g_block_size);
        return false;
    }
 
    log_info(logger, "## Escritura del bloque: %d", numero_bloque);
    return true;
}
 
/* =========================================================
 * swap_leer_bloque
 * Lee exactamente g_block_size bytes desde el bloque indicado.
 * Devuelve buffer malloc'd que el caller debe liberar, o NULL.
 * ========================================================= */
 
void *swap_leer_bloque(int32_t numero_bloque)
{
    if (!g_swap_file) {
        log_error(logger, "Archivo de SWAP no abierto");
        return NULL;
    }
 
    long offset = (long)numero_bloque * g_block_size;
    if (offset + g_block_size > g_swap_size) {
        log_error(logger,
                  "Bloque %d fuera de rango (swap_size=%d, block_size=%d)",
                  numero_bloque, g_swap_size, g_block_size);
        return NULL;
    }
 
    if (fseek(g_swap_file, offset, SEEK_SET) != 0) {
        log_error(logger, "fseek fallo en bloque %d: %s",
                  numero_bloque, strerror(errno));
        return NULL;
    }
 
    void *buf = malloc(g_block_size);
    if (!buf) return NULL;
 
    size_t leidos = fread(buf, 1, g_block_size, g_swap_file);
    if ((int32_t)leidos != g_block_size) {
        log_error(logger, "Error leyendo bloque %d: %zu/%d bytes",
                  numero_bloque, leidos, g_block_size);
        free(buf);
        return NULL;
    }
 
    log_info(logger, "## Lectura del bloque: %d", numero_bloque);
    return buf;
}

/* =========================================================
 * swap_crear_archivo
 * ========================================================= */
 
bool swap_crear_archivo(const char *path, int32_t tamanio)
{
    g_swap_file = fopen(path, "wb");
    if (!g_swap_file) {
        log_error(logger, "No se pudo crear archivo de SWAP '%s': %s",
                  path, strerror(errno));
        return false;
    }
 
    /* Expandir al tamaño configurado */
    if (fseek(g_swap_file, tamanio - 1, SEEK_SET) != 0 ||
        fwrite("", 1, 1, g_swap_file) != 1) {
        log_error(logger, "Error expandiendo archivo de SWAP a %d bytes", tamanio);
        fclose(g_swap_file);
        g_swap_file = NULL;
        return false;
    }
 
    fclose(g_swap_file);
    g_swap_file = fopen(path, "r+b");
    if (!g_swap_file) {
        log_error(logger, "No se pudo reabrir archivo de SWAP '%s': %s",
                  path, strerror(errno));
        return false;
    }
 
    g_swap_bloques_totales = tamanio / g_block_size;
 
    log_info(logger,
             "Archivo de SWAP listo: '%s' — %d bytes, %d bloques de %d bytes",
             path, tamanio, g_swap_bloques_totales, g_block_size);
    return true;
}

/* =========================================================
 * swap_atender_km
 * Hilo permanente que atiende los pedidos del KM.
 * ========================================================= */
 
void swap_atender_km(void)
{
    log_info(logger, "## Conectado a Kernel Memory — esperando operaciones");
 
    int32_t cod_op;
    while (recibir_int32(fd_km, &cod_op)) {
        switch (cod_op) {
 
            /* ── ESCRIBIR BLOQUE ── */
            case 600: {
                int32_t numero_bloque;
                if (!recibir_int32(fd_km, &numero_bloque)) {
                    log_error(logger, "Error recibiendo numero de bloque (ESCRIBIR)");
                    goto desconectar;
                }
 
                void *datos = malloc(g_block_size);
                if (!datos) {
                    enviar_int32(fd_km, 901);
                    break;
                }
 
                if (recv(fd_km, datos, g_block_size, MSG_WAITALL) != g_block_size) {
                    log_error(logger,
                              "Error recibiendo datos del bloque %d", numero_bloque);
                    free(datos);
                    enviar_int32(fd_km, 901);
                    break;
                }
 
                bool ok = swap_escribir_bloque(numero_bloque, datos);
                free(datos);
                enviar_int32(fd_km, ok ? 900 : 901);
                break;
            }
 
            /* ── LEER BLOQUE ── */
            case 601: {
                int32_t numero_bloque;
                if (!recibir_int32(fd_km, &numero_bloque)) {
                    log_error(logger, "Error recibiendo numero de bloque (LEER)");
                    goto desconectar;
                }
 
                void *datos = swap_leer_bloque(numero_bloque);
                if (!datos) {
                    enviar_int32(fd_km, 901);
                    break;
                }
 
                enviar_int32(fd_km, 900);
                send(fd_km, datos, g_block_size, MSG_NOSIGNAL);
                free(datos);
                break;
            }
 
            /* ── LIBERAR BLOQUE (extensión recomendada) ── */
            case 602: {
                int32_t numero_bloque;
                if (!recibir_int32(fd_km, &numero_bloque)) {
                    log_error(logger, "Error recibiendo numero de bloque (LIBERAR)");
                    goto desconectar;
                }
 
                /* Sobreescribir el bloque con ceros */
                void *ceros = calloc(1, g_block_size);
                if (ceros) {
                    swap_escribir_bloque(numero_bloque, ceros);
                    free(ceros);
                }
                log_info(logger, "## Bloque liberado: %d", numero_bloque);
                enviar_int32(fd_km, 900);
                break;
            }
 
            default:
                log_warning(logger, "KM — cod_op desconocido: %d", cod_op);
                break;
        }
    }
 
desconectar:
    log_warning(logger, "Kernel Memory desconectado — SWAP cerrando");
    if (g_swap_file) {
        fclose(g_swap_file);
        g_swap_file = NULL;
    }
    close(fd_km);
    fd_km = -1;
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
 
    logger = log_create("swap.log", "SWAP", true, LOG_LEVEL_INFO);
    if (!logger) { fprintf(stderr, "Error al crear logger\n"); return 1; }
 
    config = config_create(argv[1]);
    if (!config) {
        char alt[512];
        snprintf(alt, sizeof(alt), "../%s", argv[1]);
        config = config_create(alt);
    }
    if (!config) {
        log_error(logger, "No se pudo cargar '%s'", argv[1]);
        log_destroy(logger);
        return 1;
    }
 
    g_block_size = config_get_int_value(config, "BLOCK_SIZE");
    g_swap_size  = config_get_int_value(config, "SWAP_FILE_SIZE");
    char *swap_path = config_get_string_value(config, "SWAP_FILE_PATH");
 
    if (g_block_size <= 0 || g_swap_size <= 0 ||
        g_swap_size % g_block_size != 0) {
        log_error(logger,
                  "Config inválida: SWAP_FILE_SIZE=%d debe ser múltiplo de BLOCK_SIZE=%d",
                  g_swap_size, g_block_size);
        free(swap_path);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    printf("\n========== CONFIGURACIÓN SWAP ==========\n");
    printf("Archivo swap     : %s\n", swap_path);
    printf("Tamaño total     : %d bytes\n", g_swap_size);
    printf("Tamaño de bloque : %d bytes\n", g_block_size);
    printf("Bloques totales  : %d\n", g_swap_size / g_block_size);
    printf("========================================\n\n");
 
    if (!swap_crear_archivo(swap_path, g_swap_size)) {
        free(swap_path);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
    free(swap_path);
 
    char *ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");
 
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_km = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
 
    if (fd_km == -1) {
        log_error(logger, "Fallo conexion con Kernel Memory");
        if (g_swap_file) fclose(g_swap_file);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    if (!enviar_handshake(logger, fd_km, TIPO_SWAP)) {
        log_error(logger, "Handshake fallo con Kernel Memory");
        close(fd_km);
        if (g_swap_file) fclose(g_swap_file);
        config_destroy(config); log_destroy(logger);
        return 1;
    }
 
    /* Enviar metadatos al KM */
    enviar_int32(fd_km, g_block_size);
    enviar_int32(fd_km, g_swap_size);
 
    log_info(logger,
             "## Conectado a Kernel Memory — bloque: %d bytes, total: %d bytes, "
             "bloques disponibles: %d",
             g_block_size, g_swap_size, g_swap_bloques_totales);
 
    swap_atender_km();
 
    log_info(logger, "SWAP finalizando");
    if (g_swap_file) fclose(g_swap_file);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}

