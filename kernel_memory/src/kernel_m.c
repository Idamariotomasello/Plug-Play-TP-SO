#include "kernel_m.h"
#include <cliente.h>
#include <server.h>
#include <time.h>

t_config *g_config  = NULL;
t_log *g_logger = NULL;

/* Procesos e instrucciones */
t_proceso_km g_procesos[KM_MAX_PROCESOS];
bool         g_procesos_init = false;
char         g_scripts_basepath[1024] = {0};

/* Memory Sticks */
t_ms_entry  g_ms_lista[MAX_MS];
int         g_ms_count    = 0;
int32_t     g_memoria_total = 0;

/* Huecos de memoria libre */
t_hueco     g_huecos[MAX_HUECOS];
int         g_huecos_count = 0;

/* SWAP */
bool        g_swap_bloques_libres[MAX_SWAP_BLOQUES];
int32_t     g_swap_bloques_totales = 0;
int         fd_swap_global  = -1;

/* Estrategia de asignación */
e_km_strategy g_strategy = KM_STRATEGY_BEST_FIT;

int32_t g_swap_block_size = 0;   // se inicializa al conectar SWAP
 
/* Contadores de conexiones */
int g_cpus_conectadas = 0;
int g_ms_conectados   = 0;
int g_swap_conectado  = 0;
 
/* FD del Kernel Scheduler (único) */
int fd_ks_global = -1;
 

/* Gestión de instrucciones */
 
void km_init_procesos(void)
{
    if (g_procesos_init) return;
    memset(g_procesos, 0, sizeof(g_procesos));
    g_procesos_init = true;
}
 
bool km_cargar_instrucciones(int32_t pid, const char *path)
{
    km_init_procesos();
    FILE *f = fopen(path, "r");
    if (!f) {
        log_error(g_logger, "No se pudo abrir archivo de script: %s (PID=%d)", path, pid);
        return false;
    }
 
    pthread_mutex_lock(&mutex_procesos);
 
    int slot = -1;
    for (int i = 0; i < KM_MAX_PROCESOS; i++)
        if (!g_procesos[i].activo) { slot = i; break; }
 
    if (slot == -1) {
        log_error(g_logger, "No hay espacio para más procesos");
        pthread_mutex_unlock(&mutex_procesos);
        fclose(f);
        return false;
    }
 
    g_procesos[slot].pid             = pid;
    g_procesos[slot].instrucciones   = malloc(KM_MAX_INSTRUCCIONES * sizeof(char *));
    g_procesos[slot].n_instrucciones = 0;
    g_procesos[slot].activo          = true;
    memset(&g_procesos[slot].registros, 0, sizeof(t_registros));
    g_procesos[slot].n_segmentos = 0;
 
    char linea[256];
    while (fgets(linea, sizeof(linea), f) &&
           g_procesos[slot].n_instrucciones < KM_MAX_INSTRUCCIONES)
    {
        /* Ignorar comentarios (líneas que empiezan con ';') */
        char *trim = linea;
        while (*trim == ' ' || *trim == '\t') trim++;
        if (*trim == ';' || *trim == '\0') continue;
 
        int len = strlen(trim);
        while (len > 0 && (trim[len-1] == '\n' || trim[len-1] == '\r'))
            trim[--len] = '\0';
        if (len == 0) continue;
 
        g_procesos[slot].instrucciones[g_procesos[slot].n_instrucciones++]
            = strdup(trim);
    }
 
    log_info(g_logger, "## PID: %d - Instrucciones cargadas: %d",
             pid, g_procesos[slot].n_instrucciones);
 
    pthread_mutex_unlock(&mutex_procesos);
    fclose(f);
    return true;
}
 
char *km_obtener_instruccion(int32_t pid, int32_t pc)
{
    km_init_procesos();
    pthread_mutex_lock(&mutex_procesos);
    char *resultado = NULL;
 
    for (int i = 0; i < KM_MAX_PROCESOS; i++) {
        if (g_procesos[i].activo && g_procesos[i].pid == pid) {
            if (pc >= 0 && pc < g_procesos[i].n_instrucciones)
                resultado = g_procesos[i].instrucciones[pc];
            else
                log_error(g_logger, "PC fuera de rango: PID=%d PC=%d (max=%d)",
                          pid, pc, g_procesos[i].n_instrucciones);
            break;
        }
    }
 
    pthread_mutex_unlock(&mutex_procesos);
    return resultado;
}
 
int km_contar_instrucciones(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int count = 0;
    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        char *trim = linea;
        while (*trim == ' ' || *trim == '\t') trim++;
        if (*trim == ';' || *trim == '\0') continue;
        int len = strlen(trim);
        while (len > 0 && (trim[len-1] == '\n' || trim[len-1] == '\r'))
            trim[--len] = '\0';
        if (len > 0) count++;
    }
    fclose(f);
    return count;
}


/* Helpers de paquete */
 
void *km_leer_campo(void *stream, int stream_size, int *offset, int *campo_size)
{
    if (*offset + (int)sizeof(int) > stream_size) return NULL;
    memcpy(campo_size, (char *)stream + *offset, sizeof(int));
    *offset += sizeof(int);
    if (*campo_size <= 0 || *offset + *campo_size > stream_size) return NULL;
    void *dato = malloc(*campo_size);
    if (!dato) return NULL;
    memcpy(dato, (char *)stream + *offset, *campo_size);
    *offset += *campo_size;
    return dato;
}
 
bool km_recibir_cuerpo_paquete(int fd, void **stream_out, int *size_out)
{
    if (recv(fd, size_out, sizeof(int), MSG_WAITALL) != sizeof(int)) return false;
    if (*size_out <= 0) { *stream_out = NULL; return true; }
    *stream_out = malloc(*size_out);
    if (!*stream_out) return false;
    if (recv(fd, *stream_out, *size_out, MSG_WAITALL) != *size_out) {
        free(*stream_out);
        return false;
    }
    return true;
}
 
/* km_procesar_crear_proceso */
void km_procesar_crear_proceso(int fd_ks, void *stream, int stream_size)
{
    int offset = 0, campo_size;
 
    void *raw = km_leer_campo(stream, stream_size, &offset, &campo_size);
    if (!raw) { log_error(g_logger, "CREATE_PROCESS: error leyendo PID"); return; }
    int32_t pid; memcpy(&pid, raw, sizeof(int32_t)); free(raw);
 
    raw = km_leer_campo(stream, stream_size, &offset, &campo_size);
    if (!raw) { log_error(g_logger, "CREATE_PROCESS: error leyendo prioridad"); return; }
    int32_t prioridad; memcpy(&prioridad, raw, sizeof(int32_t)); free(raw);
 
    raw = km_leer_campo(stream, stream_size, &offset, &campo_size);
    if (!raw) { log_error(g_logger, "CREATE_PROCESS: error leyendo nombre script"); return; }
    char *nombre = malloc(campo_size + 1);
    memcpy(nombre, raw, campo_size);
    nombre[campo_size] = '\0';
    free(raw);
 
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", g_scripts_basepath, nombre);
    if (n < 0 || n >= (int)sizeof(path)) {
        log_error(g_logger, "Ruta demasiado larga para PID %d", pid);
        free(nombre);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
 
    log_info(g_logger, "## PID: %d - Script: %s", pid, nombre);
    log_info(g_logger, "## PID: %d - Path completo: %s", pid, path);
    log_info(g_logger, "## PID: %d - Prioridad: %d", pid, prioridad);
 
    FILE *test = fopen(path, "r");
    if (!test) {
        log_error(g_logger,
                  "## PID: %d - Script '%s' no encontrado en '%s' — respondiendo ERROR",
                  pid, nombre, path);
        free(nombre);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
    fclose(test);
 
    int n_instrucciones = km_contar_instrucciones(path);
    log_info(g_logger, "## PID: %d - Proceso Creado", pid);
    if (n_instrucciones >= 0)
        log_info(g_logger, "## PID: %d - Instrucciones: %d lineas", pid, n_instrucciones);
 
    free(nombre);
 
    if (!km_cargar_instrucciones(pid, path))
        log_error(g_logger, "Error cargando instrucciones para PID %d", pid);
 
    enviar_int32(fd_ks, OP_OK);
}


/* SECCIÓN 1 — GESTIÓN DE HUECOS */

/* km_hueco_agregar
 * Añade un nuevo hueco a la lista global.
 * Debe llamarse con mutex_huecos tomado.
 */
void km_hueco_agregar(const t_hueco *h)
{
    if (g_huecos_count >= MAX_HUECOS) {
        log_error(g_logger, "km_hueco_agregar: tabla de huecos llena");
        return;
    }
    g_huecos[g_huecos_count++] = *h;
}

/* km_mergear_huecos
 * Fusiona huecos adyacentes dentro del mismo MS.
 * Después de liberar un trozo, llama con ms_id del trozo.
 *
 * Algoritmo:
 *   1. Recopila todos los huecos activos del ms_id dado.
 *   2. Los ordena por base (burbuja; lista corta).
 *   3. Fusiona los que se tocan: [base, base+tamanio) contiguo al siguiente.
 *   4. Reescribe la lista global eliminando los fusionados.
 */
void km_mergear_huecos(int32_t ms_id)
{
    pthread_mutex_lock(&mutex_huecos);
 
    /* --- 1. copiar huecos del ms_id a arreglo temporal --- */
    t_hueco tmp[MAX_HUECOS];
    int n = 0;
    for (int i = 0; i < g_huecos_count; i++) {
        if (g_huecos[i].activo && g_huecos[i].ms_id == ms_id)
            tmp[n++] = g_huecos[i];
    }
 
    if (n < 2) {
        pthread_mutex_unlock(&mutex_huecos);
        return;
    }
 
    /* --- 2. ordenar por base (burbuja) --- */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (tmp[j].base < tmp[i].base) {
                t_hueco aux = tmp[i]; tmp[i] = tmp[j]; tmp[j] = aux;
            }
 
    /* --- 3. merge --- */
    for (int i = 0; i < n - 1; i++) {
        if (!tmp[i].activo) continue;
        if (tmp[i].base + tmp[i].tamanio == tmp[i + 1].base) {
            /* adyacentes → fusionar en i, invalidar i+1 */
            tmp[i].tamanio += tmp[i + 1].tamanio;
            tmp[i + 1].activo = false;
            log_info(g_logger,
                     "MERGE: MS=%d [%d,%d) + [%d,%d) → [%d,%d)",
                     ms_id,
                     tmp[i].base, tmp[i].base + tmp[i].tamanio - tmp[i + 1].tamanio,
                     tmp[i + 1].base, tmp[i + 1].base + tmp[i + 1].tamanio,
                     tmp[i].base, tmp[i].base + tmp[i].tamanio);
        }
    }
 
    /* --- 4. reescribir lista global --- */
    /* Eliminar todos los del ms_id de g_huecos */
    int nuevo_count = 0;
    for (int i = 0; i < g_huecos_count; i++) {
        if (g_huecos[i].activo && g_huecos[i].ms_id != ms_id)
            g_huecos[nuevo_count++] = g_huecos[i];
    }
    /* Agregar los fusionados */
    for (int i = 0; i < n; i++) {
        if (tmp[i].activo)
            g_huecos[nuevo_count++] = tmp[i];
    }
    g_huecos_count = nuevo_count;
 
    pthread_mutex_unlock(&mutex_huecos);
}

/* km_buscar_hueco
 * Devuelve puntero al hueco que mejor cumple la estrategia,
 * o NULL si no hay espacio.
 * NO toma mutex (el caller debe proteger si es necesario).
 */
t_hueco *km_buscar_hueco(int32_t tamanio)
{
    t_hueco *seleccion = NULL;
 
    for (int i = 0; i < g_huecos_count; i++) {
        t_hueco *h = &g_huecos[i];
        if (!h->activo || h->tamanio < tamanio) continue;
 
        switch (g_strategy) {
            case KM_STRATEGY_FIRST_FIT:
                return h;   /* primer ajuste: retorna de inmediato */
 
            case KM_STRATEGY_BEST_FIT:
                if (!seleccion || h->tamanio < seleccion->tamanio)
                    seleccion = h;
                break;
 
            case KM_STRATEGY_WORST_FIT:
                if (!seleccion || h->tamanio > seleccion->tamanio)
                    seleccion = h;
                break;
        }
    }
    return seleccion;
}


/* SECCIÓN 2 — ASIGNACIÓN DE SEGMENTO */
 
/* km_guardar_segmento
 * Guarda un segmento en el proceso indicado.
 */
void km_guardar_segmento(int32_t pid, const t_segmento *seg)
{
    pthread_mutex_lock(&mutex_procesos);
    for (int i = 0; i < KM_MAX_PROCESOS; i++) {
        if (g_procesos[i].activo && g_procesos[i].pid == pid) {
            g_procesos[i].segmentos[g_procesos[i].n_segmentos++] = *seg;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_procesos);
}
 
/* km_asignar_segmento
 * Intenta asignar tam_seg bytes a pid/seg_id usando la estrategia configurada.
 * Puede partir el segmento en múltiples trozos si no hay un hueco contiguo.
 *
 * Retorna true si logró asignar todo, false si no hay espacio suficiente
 * (el caller deberá disparar compactación).
 */

bool km_asignar_segmento(int32_t pid, int32_t seg_id, int32_t tam_seg)
{
    pthread_mutex_lock(&mutex_huecos);

    /* Buscar UN ÚNICO hueco contiguo que alcance */
    t_hueco *h = km_buscar_hueco(tam_seg);   /* ← tam_seg, no 1 */

    if (!h) {
        /* No hay hueco contiguo suficiente */
        pthread_mutex_unlock(&mutex_huecos);
        return false;   /* el caller sumará huecos y decidirá si compactar */
    }

    /* Asignar en un único trozo */
    t_segmento seg;
    memset(&seg, 0, sizeof(seg));
    seg.activo  = true;
    seg.seg_id  = seg_id;
    seg.limite  = tam_seg;

    t_trozo_segmento *trozo = &seg.trozos[0];
    trozo->ms_id         = h->ms_id;
    trozo->dir_fisica_ms = h->base;
    trozo->offset_seg    = 0;
    trozo->tamanio       = tam_seg;
    trozo->en_swap = false;
    trozo->num_bloques_swap = 0;
    seg.n_trozos = 1;

    h->base    += tam_seg;
    h->tamanio -= tam_seg;
    if (h->tamanio == 0) h->activo = false;

    pthread_mutex_unlock(&mutex_huecos);

    km_guardar_segmento(pid, &seg);

    log_info(g_logger, "## PID: %d - Segmento Creado %d - Tamaño: %d en 1 trozo(s)",
             pid, seg_id, tam_seg);

    return true;
}


/* SECCIÓN 3 — LIBERACIÓN DE TROZOS Y MERGE */
 
/* km_liberar_trozo
 * Libera la memoria física de un trozo.
 * Si el trozo está en swap, libera el bloque de swap.
 * Si está en MS, devuelve el espacio al pool de huecos y hace merge.
 */
void km_liberar_trozo(const t_trozo_segmento *trozo)
{
#ifdef TROZO_HAS_EN_SWAP
    if (trozo->en_swap) {
        for (int i = 0; i < trozo->num_bloques_swap; i++)
            km_swap_liberar_bloque(trozo->bloques_swap[i]);
        return;
    }
#endif
 
    t_hueco nuevo = {
        .activo  = true,
        .ms_id   = trozo->ms_id,
        .base    = trozo->dir_fisica_ms,
        .tamanio = trozo->tamanio
    };
 
    pthread_mutex_lock(&mutex_huecos);
    km_hueco_agregar(&nuevo);
    pthread_mutex_unlock(&mutex_huecos);
 
    km_mergear_huecos(trozo->ms_id);
}


/* SECCIÓN 4 — COMPACTACIÓN */

/* km_compactar_global
 * Compacta toda la memoria moviendo todos los trozos (de todos los procesos)
 * al principio de la memoria global (base 0), distribuyéndolos a lo largo de
 * de los Memory Sticks en orden. Después de esta operación:
 *   - Todos los trozos quedan contiguos desde la dirección global 0.
 *   - Cada MS queda ocupado desde su offset 0 hasta donde alcance.
 *   - La lista de huecos se reconstruye con un único hueco al final de cada MS.
 *
 * Precondición: todas las CPUs han sido desalojadas (garantizado por el KS).
 */
void km_compactar_global(void)
{
    log_info(g_logger, "## Inicio de compactación");

    pthread_mutex_lock(&mutex_huecos);
    pthread_mutex_lock(&mutex_ms_lista);
    pthread_mutex_lock(&mutex_procesos);

    // 1. Recolectar todos los trozos en memoria (no swap) de todos los procesos
    typedef struct {
        t_trozo_segmento *trozo;
        int32_t           tam;
    } t_trozo_ref;
    t_trozo_ref trozos[MAX_HUECOS]; // máximo razonable
    int n_trozos = 0;

    for (int p = 0; p < KM_MAX_PROCESOS; p++) {
        if (!g_procesos[p].activo) continue;
        for (int s = 0; s < g_procesos[p].n_segmentos; s++) {
            t_segmento *seg = &g_procesos[p].segmentos[s];
            if (!seg->activo) continue;
            for (int t = 0; t < seg->n_trozos; t++) {
                t_trozo_segmento *trozo = &seg->trozos[t];
#ifdef TROZO_HAS_EN_SWAP
                if (trozo->en_swap) continue;
#endif
                trozos[n_trozos].trozo = trozo;
                trozos[n_trozos].tam   = trozo->tamanio;
                n_trozos++;
            }
        }
    }

    // 2. Compactar en orden: cursor_global = 0
    int32_t cursor_global = 0;

    for (int i = 0; i < n_trozos; i++) {
        t_trozo_segmento *trozo = trozos[i].trozo;
        int32_t tam = trozo->tamanio;

        // Determinar en qué MS y offset debe ir el trozo según cursor_global
        int32_t ms_destino = -1;
        int32_t offset_destino = 0;
        int32_t acumulado = 0;
        for (int m = 0; m < g_ms_count; m++) {
            if (!g_ms_lista[m].activo) continue;
            if (cursor_global < acumulado + g_ms_lista[m].tamanio) {
                ms_destino = m;
                offset_destino = cursor_global - acumulado;
                break;
            }
            acumulado += g_ms_lista[m].tamanio;
        }
        if (ms_destino == -1) {
            log_error(g_logger, "Compactación: cursor_global fuera de rango");
            goto unlock;
        }

        // Si el trozo ya está en esa posición exacta, solo avanzar cursor
        if (trozo->ms_id == ms_destino && trozo->dir_fisica_ms == offset_destino) {
            cursor_global += tam;
            continue;
        }

        // Leer del origen
        int fd_origen = g_ms_lista[trozo->ms_id].fd;
        void *buf = malloc(tam);
        if (!buf) {
            log_error(g_logger, "Compactación: malloc falló");
            goto unlock;
        }

        pthread_mutex_lock(&mutex_ms_ops[trozo->ms_id]);
        enviar_int32(fd_origen, OP_LEER_MS);
        enviar_int32(fd_origen, trozo->dir_fisica_ms);
        enviar_int32(fd_origen, tam);
        int32_t resp, tam_r;
        recibir_int32(fd_origen, &resp);
        recibir_int32(fd_origen, &tam_r);
        recv(fd_origen, buf, tam_r, MSG_WAITALL);
        pthread_mutex_unlock(&mutex_ms_ops[trozo->ms_id]);

        // Escribir en el destino
        int fd_destino = g_ms_lista[ms_destino].fd;
        pthread_mutex_lock(&mutex_ms_ops[ms_destino]);
        enviar_int32(fd_destino, OP_ESCRIBIR_MS);
        enviar_int32(fd_destino, offset_destino);
        enviar_int32(fd_destino, tam);
        send(fd_destino, buf, tam, MSG_NOSIGNAL);
        recibir_int32(fd_destino, &resp);
        pthread_mutex_unlock(&mutex_ms_ops[ms_destino]);

        free(buf);

        // Actualizar el trozo
        trozo->ms_id = ms_destino;
        trozo->dir_fisica_ms = offset_destino;

        cursor_global += tam;
    }

    // 3. Reconstruir la lista de huecos
    g_huecos_count = 0;
    int32_t acum = 0;
    for (int m = 0; m < g_ms_count; m++) {
        if (!g_ms_lista[m].activo) continue;
        int32_t ocupado = 0;
        if (cursor_global > acum) {
            ocupado = (cursor_global - acum < g_ms_lista[m].tamanio) ?
                      (cursor_global - acum) : g_ms_lista[m].tamanio;
        }
        int32_t libre = g_ms_lista[m].tamanio - ocupado;
        if (libre > 0) {
            t_hueco h = {
                .activo = true,
                .ms_id = m,
                .base = ocupado,
                .tamanio = libre
            };
            g_huecos[g_huecos_count++] = h;
            log_info(g_logger,
                     "COMPACT: MS=%d hueco [%d, %d) = %d bytes",
                     m, ocupado, ocupado + libre, libre);
        }
        acum += g_ms_lista[m].tamanio;
        if (cursor_global <= acum) break; // ya no hay más datos
    }

unlock:
    pthread_mutex_unlock(&mutex_procesos);
    pthread_mutex_unlock(&mutex_ms_lista);
    pthread_mutex_unlock(&mutex_huecos);

    log_info(g_logger, "## Fin de compactación");
}


/* SECCIÓN 5 — SWAP */
 
/* km_swap_reservar_bloque
 * Devuelve el número de bloque libre, o -1 si no hay.
 */
int32_t km_swap_reservar_bloque(void)
{
    for (int32_t i = 0; i < g_swap_bloques_totales; i++) {
        if (g_swap_bloques_libres[i]) {
            g_swap_bloques_libres[i] = false;
            log_info(g_logger, "SWAP: bloque %d reservado", i);
            return i;
        }
    }
    log_error(g_logger, "SWAP: sin bloques libres");
    return -1;
}
 
/* km_swap_liberar_bloque */
void km_swap_liberar_bloque(int32_t bloque)
{
    if (bloque < 0 || bloque >= g_swap_bloques_totales) return;
    g_swap_bloques_libres[bloque] = true;
    log_info(g_logger, "SWAP: bloque %d liberado", bloque);
}
 
/* km_swap_escribir
 * Envía OP_SWAP_ESCRIBIR_BLOQUE al módulo SWAP.
 * `tamanio` debe ser <= block_size; si es menor se rellena con '\0'.
 */

 bool km_swap_escribir(int32_t bloque, void *datos, int32_t tamanio)
{
    pthread_mutex_lock(&mutex_fd_swap);
    int fd = fd_swap_global;
    if (fd == -1) {
        pthread_mutex_unlock(&mutex_fd_swap);
        log_error(g_logger, "SWAP: no conectado");
        return false;
    }

    if (!enviar_int32(fd, 600)) {
        pthread_mutex_unlock(&mutex_fd_swap);
        log_error(g_logger, "## SWAP: error enviando opcode escritura bloque %d", bloque);
        return false;
    }
    if (!enviar_int32(fd, bloque)) {
        pthread_mutex_unlock(&mutex_fd_swap);
        log_error(g_logger, "## SWAP: error enviando numero de bloque %d", bloque);
        return false;
    }
   ssize_t enviados = send(fd, datos, tamanio, MSG_NOSIGNAL);
   if (enviados != tamanio) {
       pthread_mutex_unlock(&mutex_fd_swap);
       log_error(g_logger,
                 "## SWAP: error enviando datos bloque %d (enviados=%zd/%d): %s",
                 bloque, enviados, tamanio, strerror(errno));
       return false;
    }
    int32_t resp;
    bool ok = recibir_int32(fd, &resp) && resp == 900;
    pthread_mutex_unlock(&mutex_fd_swap);

    log_info(g_logger, "## Escritura del bloque: %d (%s)", bloque, ok ? "OK" : "ERROR");
    return ok;
}
 
/* km_swap_leer
 * Envía OP_SWAP_LEER_BLOQUE al módulo SWAP y recibe datos.
 */
bool km_swap_leer(int32_t bloque, void *dest, int32_t tamanio)
{
    pthread_mutex_lock(&mutex_fd_swap);
    int fd = fd_swap_global;
    if (fd == -1) {
        pthread_mutex_unlock(&mutex_fd_swap);
        log_error(g_logger, "SWAP: no conectado");
        return false;
    }

    if (!enviar_int32(fd, 601)) {
        pthread_mutex_unlock(&mutex_fd_swap);
        log_error(g_logger, "## SWAP: error enviando opcode lectura bloque %d", bloque);
        return false;
    }
    if (!enviar_int32(fd, bloque)) {
        pthread_mutex_unlock(&mutex_fd_swap);
        log_error(g_logger, "## SWAP: error enviando numero de bloque %d", bloque);
        return false;
    }

    int32_t resp;
    if (!recibir_int32(fd, &resp) || resp != 900) {
        pthread_mutex_unlock(&mutex_fd_swap);
       log_error(g_logger,
                 "## SWAP: respuesta inesperada en lectura bloque %d (resp=%d)",
                 bloque, resp);
        return false;
    }
    ssize_t leidos = recv(fd, dest, tamanio, MSG_WAITALL);
    pthread_mutex_unlock(&mutex_fd_swap);

    bool ok = (leidos == tamanio);
   if (!ok)
       log_error(g_logger,
                 "## SWAP: error recibiendo datos bloque %d (leidos=%zd/%d): %s",
                 bloque, leidos, tamanio, strerror(errno));
    log_info(g_logger, "## Lectura del bloque: %d (%s)", bloque, ok ? "OK" : "ERROR");
    return ok;
}


/* SECCIÓN 6 — SUSPENSIÓN / DES-SUSPENSIÓN DE PROCESO */
 
/* km_suspender_proceso
 * Mueve todos los segmentos del proceso a SWAP.
 * Por cada trozo: reserva bloque, escribe datos en swap, libera espacio en MS.
 */

void km_suspender_proceso(int fd_ks, int32_t pid)
{
    log_info(g_logger, "## PID: %d - Suspendiendo proceso (moviendo a SWAP)", pid);

    pthread_mutex_lock(&mutex_procesos);

    for (int i = 0; i < KM_MAX_PROCESOS; i++) {
        if (!g_procesos[i].activo || g_procesos[i].pid != pid) continue;

        for (int s = 0; s < g_procesos[i].n_segmentos; s++) {
            t_segmento *seg = &g_procesos[i].segmentos[s];
            if (!seg->activo) continue;

            for (int t = 0; t < seg->n_trozos; t++) {
                t_trozo_segmento *trozo = &seg->trozos[t];
                if (trozo->en_swap) continue;

                void *buf = malloc(trozo->tamanio);
               if (!buf) {
                   log_error(g_logger,
                       "## PID: %d - malloc falló para trozo %d", pid, t);
                   goto suspender_error;   // ← responde OP_ERROR al KS
               }

                int fd_ms = g_ms_lista[trozo->ms_id].fd;

                pthread_mutex_lock(&mutex_ms_ops[trozo->ms_id]);
                enviar_int32(fd_ms, 700);
                enviar_int32(fd_ms, trozo->dir_fisica_ms);
                enviar_int32(fd_ms, trozo->tamanio);
                int32_t resp, tam_r;
                recibir_int32(fd_ms, &resp);
                recibir_int32(fd_ms, &tam_r);
                recv(fd_ms, buf, tam_r, MSG_WAITALL);
                pthread_mutex_unlock(&mutex_ms_ops[trozo->ms_id]);

                int32_t block_size = g_swap_block_size;
                int32_t num_bloques = (trozo->tamanio + block_size - 1) / block_size;
                int32_t bloques_reservados[MAX_BLOQUES_SWAP_POR_TROZO];
                memset(bloques_reservados, -1, sizeof(bloques_reservados));
                bool ok = true;

                for (int b = 0; b < num_bloques; b++) {
                    int32_t bloque = km_swap_reservar_bloque();
                    if (bloque == -1) {
                        ok = false;
                        break;
                    }
                    bloques_reservados[b] = bloque;
                }

                if (!ok) {
                    for (int b = 0; b < num_bloques; b++)
                       if (b < num_bloques && bloques_reservados[b] != -1)
                            km_swap_liberar_bloque(bloques_reservados[b]);
                    log_error(g_logger,
                        "## PID: %d - Error reservando bloques swap", pid);
                    free(buf);
                   
                   goto suspender_error;
                }

                for (int b = 0; b < num_bloques; b++) {
                    int32_t offset = b * block_size;
                    int32_t tam_fragmento = (b == num_bloques - 1) ?
                                            (trozo->tamanio - offset) : block_size;
                    void *frag_buf = calloc(1, block_size);
                    if (!frag_buf) {
                        ok = false;
                        log_error(g_logger,
                                  "## PID: %d - calloc falló bloque %d", pid, b);
                        break;
                    }
                    memcpy(frag_buf, (char*)buf + offset, tam_fragmento);
                    if (!km_swap_escribir(bloques_reservados[b], frag_buf, block_size)) {
                        free(frag_buf);
                        ok = false;
                        log_error(g_logger,
                                  "## PID: %d - km_swap_escribir falló en bloque %d",
                                  pid, b);
                        break;
                    }
                    free(frag_buf);
                }

                free(buf);   // ← única liberación, siempre acá independiente de ok

                if (!ok) {
                    for (int b = 0; b < num_bloques; b++)
                        if (bloques_reservados[b] != -1)
                            km_swap_liberar_bloque(bloques_reservados[b]);
                    log_error(g_logger,
                              "## PID: %d - Error escribiendo en swap", pid);
                    goto suspender_error;
                }

                // A partir de acá: ok == true, buf ya liberado, continuar con el trozo
                t_hueco h = {
                    .activo  = true,
                    .ms_id   = trozo->ms_id,
                    .base    = trozo->dir_fisica_ms,
                    .tamanio = trozo->tamanio
                };
                pthread_mutex_lock(&mutex_huecos);
                km_hueco_agregar(&h);
                pthread_mutex_unlock(&mutex_huecos);
                int ms_id_viejo = trozo->ms_id;

                trozo->en_swap = true;
                trozo->ms_id = -1;
                trozo->dir_fisica_ms = bloques_reservados[0];
                trozo->num_bloques_swap = num_bloques;
                for (int b = 0; b < num_bloques; b++)
                    trozo->bloques_swap[b] = bloques_reservados[b];

                km_mergear_huecos(ms_id_viejo);
            }
        }
        break;
    }

    pthread_mutex_unlock(&mutex_procesos);
   /* Camino exitoso: siempre responde OK */
    enviar_int32(fd_ks, OP_OK);
    log_info(g_logger, "## PID: %d - Proceso suspendido", pid);
   return;

suspender_error:
   /* Camino de error: SIEMPRE responde para no bloquear al KS */
   pthread_mutex_unlock(&mutex_procesos);
   log_error(g_logger,
       "## PID: %d - Suspensión abortada — respondiendo OP_ERROR al KS", pid);
   enviar_int32(fd_ks, OP_ERROR);
}

/* km_dessuspender_proceso
 * Restaura todos los segmentos desde SWAP a la memoria principal.
 * Usa la estrategia de búsqueda de huecos configurada.
 */
void km_dessuspender_proceso(int fd_ks, int32_t pid)
{
    log_info(g_logger, "## PID: %d - Des-suspendiendo proceso (restaurando de SWAP)", pid);
 
    pthread_mutex_lock(&mutex_procesos);
 
    for (int i = 0; i < KM_MAX_PROCESOS; i++) {
        if (!g_procesos[i].activo || g_procesos[i].pid != pid) continue;

        for (int s = 0; s < g_procesos[i].n_segmentos; s++) {
            t_segmento *seg = &g_procesos[i].segmentos[s];
            if (!seg->activo) continue;

            for (int t = 0; t < seg->n_trozos; t++) {
                t_trozo_segmento *trozo = &seg->trozos[t];
                if (!trozo->en_swap) continue;

                int32_t tamanio_total = trozo->tamanio;
                int32_t num_bloques = trozo->num_bloques_swap;
                

                // 1. Buscar hueco en MS para el trozo completo
                pthread_mutex_lock(&mutex_huecos);
                t_hueco *h = km_buscar_hueco(tamanio_total);
                if (!h) {
                    pthread_mutex_unlock(&mutex_huecos);
                    log_error(g_logger, "## PID: %d - Sin espacio para des-suspender", pid);
                    continue;
                }
                int32_t nueva_dir = h->base;
                int32_t nuevo_ms = h->ms_id;
                h->base += tamanio_total;
                h->tamanio -= tamanio_total;
                if (h->tamanio == 0) h->activo = false;
                pthread_mutex_unlock(&mutex_huecos);

                // 2. Leer bloques swap y escribir en MS
                bool ok = true;
                int32_t escritos = 0;
                int32_t block_size = g_swap_block_size;

                for (int b = 0; b < num_bloques; b++) {
                    int32_t bloque = trozo->bloques_swap[b];
                    int32_t tam_fragmento = (b == num_bloques - 1) ?
                                            (tamanio_total - escritos) : block_size;

                    void *buf = malloc(block_size);
                    if (!buf) { ok = false; break; }
                    if (!km_swap_leer(bloque, buf, block_size)) {
                        free(buf);
                        ok = false;
                        break;
                    }

                    // Escribir solo los bytes relevantes
                    if (tam_fragmento > 0) {
                        int fd_ms = g_ms_lista[nuevo_ms].fd;

                        pthread_mutex_lock(&mutex_ms_ops[nuevo_ms]);

                        enviar_int32(fd_ms, OP_ESCRIBIR_MS);
                        enviar_int32(fd_ms, nueva_dir + escritos);
                        enviar_int32(fd_ms, tam_fragmento);
                       ssize_t env = send(fd_ms, buf, tam_fragmento, MSG_NOSIGNAL);
                        int32_t resp;
                       if (env != tam_fragmento) {
                           log_error(g_logger,
                                     "## PID: %d - send a MS falló (env=%zd/%d) bloque %d",
                                     pid, env, tam_fragmento, b);
                           pthread_mutex_unlock(&mutex_ms_ops[nuevo_ms]);
                           free(buf);
                           ok = false;
                           break;
                       }
                        recibir_int32(fd_ms, &resp);

                        pthread_mutex_unlock(&mutex_ms_ops[nuevo_ms]);

                        if (resp != OP_OK) { ok = false; free(buf); break; }
                    }
                    free(buf);
                    escritos += tam_fragmento;
                }

                // 3. Liberar bloques swap (todos, aunque haya fallo parcial)
                for (int b = 0; b < num_bloques; b++)
                    km_swap_liberar_bloque(trozo->bloques_swap[b]);

                if (!ok) {
                    log_error(g_logger, "## PID: %d - Error restaurando desde swap", pid);
                    continue;
                }

                // 4. Actualizar trozo: ya no está en swap
                trozo->en_swap = false;
                trozo->ms_id = nuevo_ms;
                trozo->dir_fisica_ms = nueva_dir;
                trozo->num_bloques_swap = 0;
            }
        }
        break;
    }
    pthread_mutex_unlock(&mutex_procesos);
    enviar_int32(fd_ks, OP_OK);
    log_info(g_logger, "## PID: %d - Proceso des-suspendido", pid);
}


/* SECCIÓN 7 — FINALIZACIÓN DE PROCESO */
 
/* km_finalizar_proceso
 * Libera todos los segmentos del proceso y su estructura.
 */
void km_finalizar_proceso(int fd_ks, int32_t pid)
{
    log_info(g_logger, "## PID: %d - Finalizando proceso", pid);
 
    pthread_mutex_lock(&mutex_procesos);
 
    for (int i = 0; i < KM_MAX_PROCESOS; i++) {
        if (!g_procesos[i].activo || g_procesos[i].pid != pid) continue;
 
        for (int s = 0; s < g_procesos[i].n_segmentos; s++) {
            t_segmento *seg = &g_procesos[i].segmentos[s];
            if (!seg->activo) continue;
 
            for (int t = 0; t < seg->n_trozos; t++) {
                t_trozo_segmento *trozo = &seg->trozos[t];
                pthread_mutex_unlock(&mutex_procesos);
                km_liberar_trozo(trozo);
                pthread_mutex_lock(&mutex_procesos);
            }
            seg->activo = false;
        }
 
        /* Liberar instrucciones */
        if (g_procesos[i].instrucciones) {
            for (int j = 0; j < g_procesos[i].n_instrucciones; j++)
                free(g_procesos[i].instrucciones[j]);
            free(g_procesos[i].instrucciones);
        }
 
        g_procesos[i].activo = false;
        log_info(g_logger, "## PID: %d - Proceso finalizado", pid);
        break;
    }
 
    pthread_mutex_unlock(&mutex_procesos);
 
    if (fd_ks != -1)
        enviar_int32(fd_ks, 900 /* OP_OK */);
}


/* SECCIÓN 8 — LECTURA / ESCRITURA DE DATOS (KS como intermediario) */
 
/* Traduce dir_logica a (ms_id, dir_fisica) usando la tabla de segmentos del PID.
 * Retorna true si la traducción fue exitosa, false si SEG_FAULT.
 * seg_max_size viene del config (SEGMENT_MAX_SIZE).
 */
static bool _traducir_dir(int32_t pid, int32_t dir_logica, int32_t tamanio,
                           int32_t *ms_id_out, int32_t *dir_fisica_out,
                           int32_t seg_max_size)
{
    int32_t num_seg = dir_logica / seg_max_size;
    int32_t despl   = dir_logica % seg_max_size;
 
    for (int i = 0; i < KM_MAX_PROCESOS; i++) {
        if (!g_procesos[i].activo || g_procesos[i].pid != pid) continue;
 
        for (int s = 0; s < g_procesos[i].n_segmentos; s++) {
            t_segmento *seg = &g_procesos[i].segmentos[s];
            if (!seg->activo || seg->seg_id != num_seg) continue;
 
            if (despl + tamanio > seg->limite) return false;
 
            /* Por simplificación usamos el primer trozo que cubre el despl */
            for (int t = 0; t < seg->n_trozos; t++) {
                t_trozo_segmento *trozo = &seg->trozos[t];
                if (despl >= trozo->offset_seg &&
                    despl < trozo->offset_seg + trozo->tamanio) {
                    *ms_id_out     = trozo->ms_id;
                    *dir_fisica_out = trozo->dir_fisica_ms + (despl - trozo->offset_seg);
                    return true;
                }
            }
        }
    }
    return false;
}
 
/* km_leer_datos
 * Llamado por el KS (intermediario STDOUT):
 *   recibe pid, dir_logica, tamanio →
 *   lee del MS → envía al KS.
 */
void km_leer_datos(int fd_ks, int32_t pid, int32_t dir_logica, int32_t tamanio)
{
    int seg_max = config_get_int_value(g_config, "SEGMENT_MAX_SIZE");
 
    int32_t ms_id, dir_fisica;
    if (!_traducir_dir(pid, dir_logica, tamanio, &ms_id, &dir_fisica, seg_max)) {
        log_error(g_logger, "## PID: %d - Lectura: SEG_FAULT dir=%d tam=%d",
                  pid, dir_logica, tamanio);
        enviar_int32(fd_ks, 901 /* OP_ERROR */);
        return;
    }
 
    void *buf = malloc(tamanio);
    if (!buf) { enviar_int32(fd_ks, 901); return; }
 
    int fd_ms = g_ms_lista[ms_id].fd;

    pthread_mutex_lock(&mutex_ms_ops[ms_id]);

    enviar_int32(fd_ms, 700 /* OP_LEER_MS */);
    enviar_int32(fd_ms, dir_fisica);
    enviar_int32(fd_ms, tamanio);
 
    int32_t resp, tam_r;
    recibir_int32(fd_ms, &resp);
    recibir_int32(fd_ms, &tam_r);
    recv(fd_ms, buf, tam_r, MSG_WAITALL);

    pthread_mutex_unlock(&mutex_ms_ops[ms_id]);
 
    log_info(g_logger, "## PID: %d - Lectura - Dir. Física: %d - Tamaño: %d",
             pid, dir_fisica, tamanio);
 
    enviar_int32(fd_ks, 900);
    enviar_int32(fd_ks, tamanio);
    send(fd_ks, buf, tamanio, MSG_NOSIGNAL);
    free(buf);
}
 
/* km_escribir_datos
 * Llamado por el KS (intermediario STDIN):
 *   recibe pid, dir_logica, tamanio, datos →
 *   escribe en el MS.
 */
void km_escribir_datos(int fd_ks, int32_t pid, int32_t dir_logica,
                       int32_t tamanio, void *datos)
{
    int seg_max = config_get_int_value(g_config, "SEGMENT_MAX_SIZE");
 
    int32_t ms_id, dir_fisica;
    if (!_traducir_dir(pid, dir_logica, tamanio, &ms_id, &dir_fisica, seg_max)) {
        log_error(g_logger, "## PID: %d - Escritura: SEG_FAULT dir=%d tam=%d",
                  pid, dir_logica, tamanio);
        enviar_int32(fd_ks, 901);
        return;
    }
 
    int fd_ms = g_ms_lista[ms_id].fd;

    pthread_mutex_lock(&mutex_ms_ops[ms_id]);

    enviar_int32(fd_ms, 701 /* OP_ESCRIBIR_MS */);
    enviar_int32(fd_ms, dir_fisica);
    enviar_int32(fd_ms, tamanio);
    send(fd_ms, datos, tamanio, MSG_NOSIGNAL);
 
    int32_t resp;
    recibir_int32(fd_ms, &resp);

    pthread_mutex_unlock(&mutex_ms_ops[ms_id]);
 
    log_info(g_logger, "## PID: %d - Escritura - Dir. Física: %d - Tamaño: %d",
             pid, dir_fisica, tamanio);
 
    enviar_int32(fd_ks, resp);
}


/* SECCIÓN 9 — km_atender_ms ACTUALIZADO
(reemplaza la anterior versión para que inicialice huecos correctamente)
*/

void km_atender_ms_full(t_log *logger, int fd_ms)
{
    int32_t tamanio;
    if (!recibir_int32(fd_ms, &tamanio)) {
        log_error(logger, "Error recibiendo tamaño de Memory Stick");
        close(fd_ms); return;
    }
 
    int32_t largo_ip;
    char ip_ms[64] = {0};
    int32_t puerto_ms_cpus;
 
    if (!recibir_int32(fd_ms, &largo_ip) || largo_ip <= 0 || largo_ip >= 64) {
        log_error(logger, "Error recibiendo IP del MS");
        close(fd_ms); return;
    }
    recv(fd_ms, ip_ms, largo_ip, MSG_WAITALL);
    ip_ms[largo_ip] = '\0';
 
    if (!recibir_int32(fd_ms, &puerto_ms_cpus)) {
        log_error(logger, "Error recibiendo puerto del MS");
        close(fd_ms); return;
    }
 
    pthread_mutex_lock(&mutex_ms_lista);
    pthread_mutex_lock(&mutex_huecos);
 
    int idx = g_ms_count;
    if (idx < MAX_MS) {
        g_ms_lista[idx].fd          = fd_ms;
        g_ms_lista[idx].tamanio     = tamanio;
        g_ms_lista[idx].base_global = g_memoria_total;
        g_ms_lista[idx].activo      = true;
        snprintf(g_ms_lista[idx].ip, sizeof(g_ms_lista[idx].ip), "%s", ip_ms);
        g_ms_lista[idx].puerto_cpus = puerto_ms_cpus;
        g_ms_count++;
        g_memoria_total += tamanio;
 
        /* Inicializar hueco: todo el MS está libre */
        t_hueco h = {
            .activo  = true,
            .ms_id   = idx,
            .base    = 0,
            .tamanio = tamanio
        };
        g_huecos[g_huecos_count++] = h;
 
        log_info(logger,
                 "## Memory Stick de %d bytes Conectada (MS %d, ip=%s, puerto=%d, "
                 "base_global=%d, memoria_total=%d)",
                 tamanio, idx, ip_ms, puerto_ms_cpus,
                 g_ms_lista[idx].base_global, g_memoria_total);
    }
 
    pthread_mutex_unlock(&mutex_huecos);
    pthread_mutex_unlock(&mutex_ms_lista);
 
    /* Notificar al KS */
    pthread_mutex_lock(&mutex_fd_ks);
    if (fd_ks_global != -1) {
        enviar_int32(fd_ks_global, OP_NUEVA_MEMORIA);
        enviar_int32(fd_ks_global, g_memoria_total);
    }
    pthread_mutex_unlock(&mutex_fd_ks);
 
    __atomic_add_fetch(&g_ms_conectados, 1, __ATOMIC_SEQ_CST);
    if (g_ms_conectados == 1)
        sem_post(&g_sem_listo);

    /* El hilo termina sin cerrar el socket ni esperar desconexión.
     * La desconexión se detectará cuando una operación falle. */
    // No se cierra fd_ms aquí.
}


/* _km_responder_segmento_ok
 * Busca el segmento recién creado en el proceso y envía al KS:
 *   OP_OK | n_trozos | [ms_id, dir_fisica, offset_seg, tamanio] × n_trozos
 */
void _km_responder_segmento_ok(int fd_ks, int32_t pid, int32_t seg_id)
{
    pthread_mutex_lock(&mutex_procesos);
 
    t_segmento *seg_encontrado = NULL;
    for (int i = 0; i < KM_MAX_PROCESOS && !seg_encontrado; i++) {
        if (!g_procesos[i].activo || g_procesos[i].pid != pid) continue;
        for (int s = 0; s < g_procesos[i].n_segmentos; s++) {
            if (g_procesos[i].segmentos[s].activo &&
                g_procesos[i].segmentos[s].seg_id == seg_id) {
                seg_encontrado = &g_procesos[i].segmentos[s];
                break;
            }
        }
    }
 
    if (!seg_encontrado) {
        pthread_mutex_unlock(&mutex_procesos);
        log_error(g_logger,
                  "## PID: %d - No se encontró seg %d para responder OK", pid, seg_id);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
 
    /* Log obligatorio del enunciado */
    log_info(g_logger, "## PID: %d - Segmento Creado %d - Tamaño: %d",
             pid, seg_id, seg_encontrado->limite);
 
    enviar_int32(fd_ks, OP_OK);
    enviar_int32(fd_ks, seg_encontrado->n_trozos);
 
    for (int i = 0; i < seg_encontrado->n_trozos; i++) {
        t_trozo_segmento *t = &seg_encontrado->trozos[i];
        enviar_int32(fd_ks, t->ms_id);
        enviar_int32(fd_ks, t->dir_fisica_ms);
        enviar_int32(fd_ks, t->offset_seg);
        enviar_int32(fd_ks, t->tamanio);
        log_info(g_logger,
                 "## PID: %d - Segmento %d trozo %d: MS=%d dir_fisica=%d offset=%d tam=%d",
                 pid, seg_id, i,
                 t->ms_id, t->dir_fisica_ms, t->offset_seg, t->tamanio);
    }
 
    pthread_mutex_unlock(&mutex_procesos);
}


void km_crear_segmento(int fd_ks, int32_t pid, int32_t seg_id, int32_t tam_seg)
{
    log_info(g_logger, "## PID: %d - Crear segmento %d de %d bytes", pid, seg_id, tam_seg);

    /* ── Intento 1: asignar directamente ── */
    if (km_asignar_segmento(pid, seg_id, tam_seg)) {
        log_info(g_logger, "## PID: %d - Segmento %d asignado sin compactación", pid, seg_id);
        _km_responder_segmento_ok(fd_ks, pid, seg_id);
        return;
    }

    log_info(g_logger, "## PID: %d - Asignación directa falló para segmento %d (tam=%d)", pid, seg_id, tam_seg);

    /* ── ¿Hay memoria total suficiente aunque fragmentada? ── */
    int32_t libre_total = 0;
    pthread_mutex_lock(&mutex_huecos);
    for (int i = 0; i < g_huecos_count; i++)
        if (g_huecos[i].activo) libre_total += g_huecos[i].tamanio;
    pthread_mutex_unlock(&mutex_huecos);

    log_info(g_logger, "## PID: %d - Análisis de memoria: necesita=%d, libre_total=%d, huecos=%d",
             pid, tam_seg, libre_total, g_huecos_count);

    if (libre_total < tam_seg) {
        log_error(g_logger,
                  "## PID: %d - Sin memoria para segmento %d (necesita=%d, libre_total=%d)",
                  pid, seg_id, tam_seg, libre_total);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }

    /* ── Notificar al KS que debe desalojar CPUs para compactar ── */
    log_info(g_logger,
             "## PID: %d - Memoria fragmentada (necesita=%d, libre=%d) — solicitando compactación al KS",
             pid, tam_seg, libre_total);

    pthread_mutex_lock(&mutex_fd_ks);
    if (fd_ks_global == -1) {
        pthread_mutex_unlock(&mutex_fd_ks);
        log_error(g_logger, "## KS no conectado — no se puede compactar");
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
    enviar_int32(fd_ks_global, OP_INICIAR_COMPACT);
    pthread_mutex_unlock(&mutex_fd_ks);

    /* ── Esperar confirmación de desalojo (OP_CONFIRMAR_DESALOJO) ── */
    log_info(g_logger, "## PID: %d - Esperando OP_CONFIRMAR_DESALOJO del KS", pid);
    int32_t confirm;
    if (!recibir_int32(fd_ks, &confirm)) {
        log_error(g_logger, "## PID: %d - Error recibiendo respuesta del KS", pid);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
    if (confirm != OP_CONFIRMAR_DESALOJO) {
        log_error(g_logger,
                  "## PID: %d - No se recibió confirmación de desalojo (got=%d, expected=%d)",
                  pid, confirm, OP_CONFIRMAR_DESALOJO);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
    log_info(g_logger, "## PID: %d - Confirmación de desalojo recibida — iniciando compactación", pid);

    /* ── Compactar globalmente ── */
    km_compactar_global();  // ahora es la única compactación

    /* ── Notificar fin de compactación al KS ── */
    pthread_mutex_lock(&mutex_fd_ks);
    if (fd_ks_global != -1)
        enviar_int32(fd_ks_global, OP_FIN_COMPACT);
    pthread_mutex_unlock(&mutex_fd_ks);

    /* ── Reintentar asignación después de compactar ── */
    if (!km_asignar_segmento(pid, seg_id, tam_seg)) {
        log_error(g_logger,
                  "## PID: %d - Sin memoria incluso tras compactación (seg=%d, tam=%d)",
                  pid, seg_id, tam_seg);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }

    _km_responder_segmento_ok(fd_ks, pid, seg_id);
}

/* km_eliminar_segmento */
void km_eliminar_segmento(int fd_ks, int32_t pid, int32_t seg_id)
{
    log_info(g_logger, "## PID: %d - Eliminar segmento %d", pid, seg_id);
 
    pthread_mutex_lock(&mutex_procesos);
 
    bool encontrado = false;
    for (int i = 0; i < KM_MAX_PROCESOS && !encontrado; i++) {
        if (!g_procesos[i].activo || g_procesos[i].pid != pid) continue;
 
        for (int j = 0; j < g_procesos[i].n_segmentos; j++) {
            t_segmento *seg = &g_procesos[i].segmentos[j];
            if (!seg->activo || seg->seg_id != seg_id) continue;
 
            /* Liberar cada trozo */
            for (int k = 0; k < seg->n_trozos; k++) {
                t_trozo_segmento *trozo = &seg->trozos[k];
                log_info(g_logger,
                         "## PID: %d - Liberando trozo: MS=%d dir_fisica=%d bytes=%d",
                         pid, trozo->ms_id, trozo->dir_fisica_ms, trozo->tamanio);
 
                /* Necesitamos soltar mutex_procesos para llamar km_liberar_trozo
                 * (que a su vez toma mutex_huecos) — copiamos el trozo */
                t_trozo_segmento copia = *trozo;
                pthread_mutex_unlock(&mutex_procesos);
                km_liberar_trozo(&copia);   /* definida en kernel_m_memory.c */
                pthread_mutex_lock(&mutex_procesos);
            }
 
            seg->activo   = false;
            seg->n_trozos = 0;
            encontrado    = true;
 
            log_info(g_logger,
                     "## PID: %d - Segmento %d eliminado (%d bytes liberados)",
                     pid, seg_id, seg->limite);
            break;
        }
    }
 
    pthread_mutex_unlock(&mutex_procesos);
 
    if (!encontrado) {
        log_error(g_logger,
                  "## PID: %d - Segmento %d no encontrado para eliminar", pid, seg_id);
        enviar_int32(fd_ks, OP_ERROR);
        return;
    }
 
    enviar_int32(fd_ks, OP_OK);
}


/* Servidor y despachador de clientes */
void km_iniciar_servidor(t_log *logger, int puerto)
{
    int fd_escucha = iniciar_servidor(logger, puerto);
    if (fd_escucha == -1) {
        log_error(logger, "Error iniciando servidor en puerto %d", puerto);
        return;
    }
    log_info(logger, "Kernel Memory esperando conexiones en puerto %d", puerto);
 
    while (1) {
        int fd_cliente = accept(fd_escucha, NULL, NULL);
        if (fd_cliente == -1) continue;
 
        int32_t tipo = recibir_handshake(logger, fd_cliente);
        if (tipo == HANDSHAKE_ERR) { close(fd_cliente); continue; }
 
        t_km_hilo_arg *arg = malloc(sizeof(t_km_hilo_arg));
        arg->logger    = logger;
        arg->fd_cliente = fd_cliente;
        arg->tipo      = tipo;
 
        pthread_t hilo;
        pthread_create(&hilo, NULL, km_despachar_cliente, arg);
        pthread_detach(hilo);
    }
    close(fd_escucha);
}
 
void *km_despachar_cliente(void *varg)
{
    t_km_hilo_arg *arg = (t_km_hilo_arg *)varg;
    t_log  *logger = arg->logger;
    int     fd     = arg->fd_cliente;
    int32_t tipo   = arg->tipo;
    free(arg);
 
    switch (tipo) {
        case TIPO_KS:   km_atender_ks(logger, fd);         break;
        case TIPO_CPU:  km_atender_cpu(logger, fd);        break;
        case TIPO_MS:   km_atender_ms_full(logger, fd);    break;  /* ← versión completa */
        case TIPO_SWAP: km_atender_swap(logger, fd);       break;
        default:
            log_warning(logger, "Tipo de cliente desconocido: %d (fd=%d)", tipo, fd);
            close(fd);
    }
    return NULL;
}


/* km_atender_ks — handler del Kernel Scheduler */
 
void km_atender_ks(t_log *logger, int fd_ks)
{
    log_info(logger, "## Kernel Scheduler Conectado - FD del socket: %d", fd_ks);
 
    pthread_mutex_lock(&mutex_fd_ks);
    fd_ks_global = fd_ks;
    pthread_mutex_unlock(&mutex_fd_ks);
 
    int32_t cod_op;
    while (recibir_int32(fd_ks, &cod_op)) {
        switch (cod_op) {
 
            case OP_CREAR_PROCESO: {
                void *stream = NULL; int size = 0;
                if (km_recibir_cuerpo_paquete(fd_ks, &stream, &size)) {
                    km_procesar_crear_proceso(fd_ks, stream, size);
                    free(stream);
                }
                break;
            }
 
            case OP_CREAR_SEGMENTO: {
                int32_t pid, seg_id, tam_seg;
                recibir_int32(fd_ks, &pid);
                recibir_int32(fd_ks, &seg_id);
                recibir_int32(fd_ks, &tam_seg);
                km_crear_segmento(fd_ks, pid, seg_id, tam_seg);
                break;
            }
 
            case OP_ELIMINAR_SEGMENTO: {
                int32_t pid, seg_id;
                recibir_int32(fd_ks, &pid);
                recibir_int32(fd_ks, &seg_id);
                km_eliminar_segmento(fd_ks, pid, seg_id);
                break;
            }
 
            case OP_FINALIZAR_PROCESO: {
                int32_t pid;
                recibir_int32(fd_ks, &pid);
                km_finalizar_proceso(fd_ks, pid);   /* kernel_m_memory.c */
                break;
            }
 
            case OP_SUSPENDER_PROCESO: {
                int32_t pid;
                recibir_int32(fd_ks, &pid);
                km_suspender_proceso(fd_ks, pid);   /* kernel_m_memory.c */
                break;
            }
 
            case OP_DESSUSPENDER_PROCESO: {
                int32_t pid;
                recibir_int32(fd_ks, &pid);
                km_dessuspender_proceso(fd_ks, pid); /* kernel_m_memory.c */
                break;
            }
 
            case OP_LEER_DATOS: {
                int32_t pid, dir_logica, tamanio;
                recibir_int32(fd_ks, &pid);
                recibir_int32(fd_ks, &dir_logica);
                recibir_int32(fd_ks, &tamanio);
                km_leer_datos(fd_ks, pid, dir_logica, tamanio); /* kernel_m_memory.c */
                break;
            }
 
            case OP_ESCRIBIR_DATOS: {
                int32_t pid, dir_logica, tamanio;
                recibir_int32(fd_ks, &pid);
                recibir_int32(fd_ks, &dir_logica);
                recibir_int32(fd_ks, &tamanio);
                void *datos = malloc(tamanio);
                if (datos) {
                    recv(fd_ks, datos, tamanio, MSG_WAITALL);
                    km_escribir_datos(fd_ks, pid, dir_logica, tamanio, datos);
                    free(datos);
                } else {
                    enviar_int32(fd_ks, OP_ERROR);
                }
                break;
            }
 
            case OP_CONFIRMAR_DESALOJO:
                /* El KS puede mandar esto en respuesta a OP_INICIAR_COMPACT;
                 * en el flujo normal lo recibe km_crear_segmento directamente
                 * vía recibir_int32(fd_ks, &confirm). No debería llegar aquí,
                 * pero si llega lo ignoramos silenciosamente. */
                log_info(logger, "## KS — OP_CONFIRMAR_DESALOJO recibido (fuera de contexto)");
                break;
 
            default:
                log_warning(logger, "Operación KS desconocida: %d", cod_op);
                break;
        }
    }
 
    pthread_mutex_lock(&mutex_fd_ks);
    fd_ks_global = -1;
    pthread_mutex_unlock(&mutex_fd_ks);
 
    log_warning(logger, "Kernel Scheduler desconectado (fd=%d)", fd_ks);
    close(fd_ks);
}


/* km_atender_cpu — handler de la CPU */
void km_atender_cpu(t_log *logger, int fd_cpu)
{
    int32_t id_cpu;
    if (!recibir_int32(fd_cpu, &id_cpu)) {
        log_error(logger, "Error recibiendo ID de CPU (fd=%d)", fd_cpu);
        close(fd_cpu); return;
    }
 
    __atomic_add_fetch(&g_cpus_conectadas, 1, __ATOMIC_SEQ_CST);
    log_info(logger, "## CPU %d Conectada (fd=%d) — total: %d",
             id_cpu, fd_cpu, g_cpus_conectadas);
 
    if (g_cpus_conectadas == 1) sem_post(&g_sem_listo);
 
    int32_t cod_op;
    while (recibir_int32(fd_cpu, &cod_op)) {
        switch (cod_op) {
 
            case OP_FETCH_INSTRUCCION: {
                int32_t pid, pc;
                if (!recibir_int32(fd_cpu, &pid)) goto cpu_desconectada;
                if (!recibir_int32(fd_cpu, &pc))  goto cpu_desconectada;
 
                log_info(logger, "CPU %d solicita FETCH: PID=%d, PC=%d", id_cpu, pid, pc);
 
                char *instr = km_obtener_instruccion(pid, pc);
                if (!instr) {
                    log_error(logger, "FETCH ERROR: PID=%d PC=%d no encontrado", pid, pc);
                    enviar_int32(fd_cpu, OP_ERROR);
                    break;
                }
 
                int instruction_delay = config_get_int_value(g_config, "INSTRUCTION_DELAY");
                if (instruction_delay > 0)
                    usleep((useconds_t)instruction_delay * 1000);
 
                log_info(logger, "## PID: %d - Obtener instruccion: %d - Instruccion: %s",
                         pid, pc, instr);
 
                int32_t tam = (int32_t)strlen(instr);
                if (!enviar_int32(fd_cpu, OP_OK))  goto cpu_desconectada;
                if (!enviar_int32(fd_cpu, tam))     goto cpu_desconectada;
                if (send(fd_cpu, instr, tam, MSG_NOSIGNAL) != tam) goto cpu_desconectada;
                break;
            }
 
            case OP_GET_CONTEXTO: {
                int32_t pid;
                if (!recibir_int32(fd_cpu, &pid)) goto cpu_desconectada;
 
                t_registros regs;
                int32_t     n_seg = 0;
                t_segmento  segs[MAX_SEGMENTOS];
                memset(&regs, 0, sizeof(regs));
 
                pthread_mutex_lock(&mutex_procesos);
                for (int i = 0; i < KM_MAX_PROCESOS; i++) {
                    if (g_procesos[i].activo && g_procesos[i].pid == pid) {
                        memcpy(&regs, &g_procesos[i].registros, sizeof(t_registros));
                        n_seg = g_procesos[i].n_segmentos;
                        memcpy(segs, g_procesos[i].segmentos, n_seg * sizeof(t_segmento));
                        break;
                    }
                }
                pthread_mutex_unlock(&mutex_procesos);
 
                log_info(logger, "GET_CONTEXTO: PID=%d PC=%u segmentos=%d",
                         pid, regs.PC, n_seg);
 
                if (!enviar_int32(fd_cpu, OP_OK)) goto cpu_desconectada;
                send(fd_cpu, &regs,  sizeof(t_registros), MSG_NOSIGNAL);
                enviar_int32(fd_cpu, n_seg);
                if (n_seg > 0)
                    send(fd_cpu, segs, n_seg * sizeof(t_segmento), MSG_NOSIGNAL);
                break;
            }
 
            case OP_GET_SEGMENT_MAX_SIZE: {
                int32_t seg_max_size = config_get_int_value(g_config, "SEGMENT_MAX_SIZE");
                if (seg_max_size <= 0) {
                    enviar_int32(fd_cpu, OP_ERROR);
                } else {
                    enviar_int32(fd_cpu, OP_OK);
                    enviar_int32(fd_cpu, seg_max_size);
                }
                break;
            }
 
            case OP_SET_CONTEXTO: {
                int32_t pid;
                if (!recibir_int32(fd_cpu, &pid)) goto cpu_desconectada;
 
                t_registros regs;
                int32_t     n_seg;
                t_segmento  segs[MAX_SEGMENTOS];
 
                recv(fd_cpu, &regs, sizeof(t_registros), MSG_WAITALL);
                recibir_int32(fd_cpu, &n_seg);
                if (n_seg > 0)
                    recv(fd_cpu, segs, n_seg * sizeof(t_segmento), MSG_WAITALL);
 
                pthread_mutex_lock(&mutex_procesos);
                for (int i = 0; i < KM_MAX_PROCESOS; i++) {
                    if (g_procesos[i].activo && g_procesos[i].pid == pid) {
                        memcpy(&g_procesos[i].registros, &regs, sizeof(t_registros));
                        g_procesos[i].n_segmentos = n_seg;
                        memcpy(g_procesos[i].segmentos, segs, n_seg * sizeof(t_segmento));
                        break;
                    }
                }
                pthread_mutex_unlock(&mutex_procesos);
 
                log_info(logger, "SET_CONTEXTO: PID=%d PC=%u segmentos=%d",
                         pid, regs.PC, n_seg);
                enviar_int32(fd_cpu, OP_OK);
                break;
            }
 
            case OP_GET_MS_LIST: {
                pthread_mutex_lock(&mutex_ms_lista);
                int32_t n_activos = 0;
                for (int i = 0; i < g_ms_count; i++)
                    if (g_ms_lista[i].activo) n_activos++;

                enviar_int32(fd_cpu, n_activos);

                for (int i = 0; i < g_ms_count; i++) {
                    if (!g_ms_lista[i].activo) continue;
                    // 1. Enviar ms_id (índice interno de KM)
                    enviar_int32(fd_cpu, i);
                    // 2. IP
                    int32_t largo = (int32_t)strlen(g_ms_lista[i].ip);
                    enviar_int32(fd_cpu, largo);
                    send(fd_cpu, g_ms_lista[i].ip, largo, MSG_NOSIGNAL);
                    // 3. Puerto
                    enviar_int32(fd_cpu, g_ms_lista[i].puerto_cpus);
                    // 4. Tamaño
                    enviar_int32(fd_cpu, g_ms_lista[i].tamanio);
                    log_info(logger, "CPU %d — GET_MS_LIST: MS %d ip=%s puerto=%d tam=%d",
                            id_cpu, i, g_ms_lista[i].ip,
                            g_ms_lista[i].puerto_cpus, g_ms_lista[i].tamanio);
                }
                pthread_mutex_unlock(&mutex_ms_lista);
                break;
            }
 
            default:
                log_warning(logger, "CPU %d — op_code desconocido: %d", id_cpu, cod_op);
                break;
        }
    }
 
cpu_desconectada:
    __atomic_sub_fetch(&g_cpus_conectadas, 1, __ATOMIC_SEQ_CST);
    log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd_cpu);
    close(fd_cpu);
}

/* km_atender_swap — handler del módulo SWAP */ 
void km_atender_swap(t_log *logger, int fd_swap)
{
    int32_t block_size, swap_size;
    if (!recibir_int32(fd_swap, &block_size) ||
        !recibir_int32(fd_swap, &swap_size)) {
        log_error(logger, "Error recibiendo parámetros de SWAP");
        close(fd_swap); return;
    }

    g_swap_block_size = block_size;
 
    int32_t bloques_totales = (block_size > 0) ? swap_size / block_size : 0;
 
    /* Registrar el fd y el mapa de bloques */
    pthread_mutex_lock(&mutex_fd_swap);
    fd_swap_global         = fd_swap;
    g_swap_bloques_totales = bloques_totales;
    for (int i = 0; i < bloques_totales && i < MAX_SWAP_BLOQUES; i++)
        g_swap_bloques_libres[i] = true;
    pthread_mutex_unlock(&mutex_fd_swap);
 
    __atomic_add_fetch(&g_swap_conectado, 1, __ATOMIC_SEQ_CST);
    log_info(logger,
             "## Conectado a SWAP (fd=%d) — bloque: %d bytes, total: %d bytes, "
             "bloques disponibles: %d",
             fd_swap, block_size, swap_size, bloques_totales);
 
    if (g_swap_conectado == 1) sem_post(&g_sem_listo);
 
    /* Monitorear desconexión (bloqueante) */
    char probe;
    if (recv(fd_swap, &probe, 1, MSG_WAITALL) <= 0)
        log_warning(logger, "SWAP desconectado");
 
    pthread_mutex_lock(&mutex_fd_swap);
    fd_swap_global = -1;
    pthread_mutex_unlock(&mutex_fd_swap);
 
    __atomic_sub_fetch(&g_swap_conectado, 1, __ATOMIC_SEQ_CST);
    close(fd_swap);
}
 
/* Hilo servidor */
void *hilo_servidor(void *varg)
{
    t_servidor_arg *arg = (t_servidor_arg *)varg;
    km_iniciar_servidor(arg->logger, arg->puerto);
    free(arg);
    return NULL;
}


/* main */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <archivo_config>\n", argv[0]);
        return 1;
    }
 
    /* ── Logger ── */
    g_logger = log_create("kernel_memory.log", "KERNEL_MEMORY", true, LOG_LEVEL_DEBUG);
    if (!g_logger) { fprintf(stderr, "Error al crear logger\n"); return 1; }
 
    /* ── Config ── */
    g_config = config_create(argv[1]);
    if (!g_config) {
        char ruta_alt[512];
        snprintf(ruta_alt, sizeof(ruta_alt), "../%s", argv[1]);
        g_config = config_create(ruta_alt);
    }
    if (!g_config) {
        log_error(g_logger, "No se pudo cargar config '%s'", argv[1]);
        log_destroy(g_logger);
        return 1;
    }
 
    /* ── Leer parámetros de configuración ── */
    int   km_puerto        = config_get_int_value(g_config,    "PUERTO_ESCUCHA");
    char *km_scripts_path  = config_get_string_value(g_config, "SCRIPTS_BASEPATH");
    char *km_alloc_strat   = config_get_string_value(g_config, "ALLOCATION_STRATEGY");
    int   km_instr_delay   = config_get_int_value(g_config,    "INSTRUCTION_DELAY");
    int   km_seg_max       = config_get_int_value(g_config,    "SEGMENT_MAX_SIZE");
    int   km_compact_delay = config_get_int_value(g_config,    "COMPACTION_DELAY");
 
    /* ── Guardar basepath antes de cualquier free ── */
    snprintf(g_scripts_basepath, sizeof(g_scripts_basepath), "%s", km_scripts_path);
 
    /* ── Cargar estrategia de asignación ── */
    if (km_alloc_strat == NULL || strcmp(km_alloc_strat, "BEST") == 0) {
        g_strategy = KM_STRATEGY_BEST_FIT;
    } else if (strcmp(km_alloc_strat, "WORST") == 0) {
        g_strategy = KM_STRATEGY_WORST_FIT;
    } else if (strcmp(km_alloc_strat, "FIRST") == 0) {
        g_strategy = KM_STRATEGY_FIRST_FIT;
    } else {
        log_warning(g_logger,
                    "ALLOCATION_STRATEGY='%s' desconocida — usando BEST_FIT",
                    km_alloc_strat);
        g_strategy = KM_STRATEGY_BEST_FIT;
    }
 
    const char *nombre_estrategia =
        (g_strategy == KM_STRATEGY_BEST_FIT)  ? "BEST_FIT"  :
        (g_strategy == KM_STRATEGY_WORST_FIT) ? "WORST_FIT" : "FIRST_FIT";
 
    /* ── Mostrar configuración ── */
    printf("\n========== CONFIGURACIÓN DEL KERNEL MEMORY ==========\n");
    printf("Puerto de escucha          : %d\n",  km_puerto);
    printf("Scripts base path          : %s\n",  km_scripts_path);
    printf("Estrategia de asignación   : %s → %s\n", km_alloc_strat, nombre_estrategia);
    printf("Instruction delay (ms)     : %d\n",  km_instr_delay);
    printf("Segment max size           : %d\n",  km_seg_max);
    printf("Compaction delay (ms)      : %d\n",  km_compact_delay);
    printf("=====================================================\n\n");
 
    log_info(g_logger,
             "Estrategia de asignación: %s (config='%s')",
             nombre_estrategia, km_alloc_strat ? km_alloc_strat : "NULL");
 
    /* ── Inicializar estructuras de memoria ── */
    km_init_procesos();
    memset(g_huecos,              0, sizeof(g_huecos));
    memset(g_swap_bloques_libres, 0, sizeof(g_swap_bloques_libres));
    g_huecos_count         = 0;
    g_swap_bloques_totales = 0;
    fd_swap_global         = -1;
    fd_ks_global           = -1;
    g_ms_count             = 0;
    g_memoria_total        = 0;
 
    /* ── Semáforos ── */
    inicializar_semaforos();
 
    /* ── Lanzar servidor en hilo propio ── */
    log_info(g_logger, "Kernel Memory iniciando en puerto %d", km_puerto);
 
    t_servidor_arg *arg = malloc(sizeof(t_servidor_arg));
    arg->logger = g_logger;
    arg->puerto = km_puerto;
 
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, hilo_servidor, arg) != 0) {
        log_error(g_logger, "Error creando hilo servidor");
        free(arg);
        config_destroy(g_config);
        log_destroy(g_logger);
        destruir_semaforos();
        return 1;
    }
 
    pthread_join(hilo, NULL);
 
    /* ── Limpieza ── */
    config_destroy(g_config);
    log_destroy(g_logger);
    destruir_semaforos();
    return 0;
}

