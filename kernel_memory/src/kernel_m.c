#include "kernel_m.h"
#include <cliente.h>
#include <server.h>
#include <time.h>

t_config *g_config  = NULL;
t_log *g_logger = NULL;

t_proceso_km g_procesos[KM_MAX_PROCESOS];
bool g_procesos_init = false;

int g_cpus_conectadas = 0;
int g_ms_conectados   = 0;
int g_swap_conectado  = 0;

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

    g_procesos[slot].pid = pid;
    g_procesos[slot].instrucciones = malloc(KM_MAX_INSTRUCCIONES * sizeof(char*));
    g_procesos[slot].n_instrucciones = 0;
    g_procesos[slot].activo = true;

    char linea[256];
    while (fgets(linea, sizeof(linea), f) &&
           g_procesos[slot].n_instrucciones < KM_MAX_INSTRUCCIONES)
    {
        int len = strlen(linea);
        while (len > 0 && (linea[len-1] == '\n' || linea[len-1] == '\r'))
            linea[--len] = '\0';
        if (len == 0) continue;
        g_procesos[slot].instrucciones[g_procesos[slot].n_instrucciones++]
            = strdup(linea);
    }

    log_info(g_logger, "## PID: %d - Instrucciones cargadas: %d", pid, g_procesos[slot].n_instrucciones);
    
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
            log_info(g_logger, "Buscando instrucción PID=%d, PC=%d (total=%d)", pid, pc, g_procesos[i].n_instrucciones);
            if (pc >= 0 && pc < g_procesos[i].n_instrucciones) {
                resultado = g_procesos[i].instrucciones[pc];
            } else {
                log_error(g_logger, "PC fuera de rango: PID=%d PC=%d (max=%d)", pid, pc, g_procesos[i].n_instrucciones);
            }
            break;
        }
    }
    
    pthread_mutex_unlock(&mutex_procesos);
    return resultado;
}

void *km_leer_campo(void *stream, int stream_size,
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

bool km_recibir_cuerpo_paquete(int fd, void **stream_out, int *size_out)
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

int km_contar_instrucciones(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int count = 0;
    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        int len = strlen(linea);
        while (len > 0 && (linea[len-1] == '\n' || linea[len-1] == '\r'))
            linea[--len] = '\0';
        if (len > 0) count++;
    }
    fclose(f);
    return count;
}

void km_procesar_crear_proceso(int fd_ks, void *stream, int stream_size)
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

    /* Logs obligatorios del enunciado */
    log_info(g_logger, "## PID: %d - Proceso Creado", pid);
    log_info(g_logger, "## PID: %d - Script: %s", pid, nombre);
    log_info(g_logger, "## PID: %d - Path completo: %s", pid, path);
    log_info(g_logger, "## PID: %d - Prioridad: %d", pid, prioridad);

    if (n_instrucciones >= 0)
        log_info(g_logger, "## PID: %d - Instrucciones: %d lineas", pid, n_instrucciones);

    free(nombre);

    /* Cargar instrucciones en memoria para fetch posterior */
    if (!km_cargar_instrucciones(pid, path))
        log_error(g_logger, "Error cargando instrucciones para PID %d", pid);

    /* Responder OK al KS */
    enviar_int32(fd_ks, OP_OK);
}

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
        if (tipo == HANDSHAKE_ERR) {
            close(fd_cliente);
            continue;
        }

        t_km_hilo_arg *arg = malloc(sizeof(t_km_hilo_arg));
        arg->logger = logger;
        arg->fd_cliente = fd_cliente;
        arg->tipo = tipo;

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
        case TIPO_KS:  km_atender_ks(logger, fd);   break;
        case TIPO_CPU: km_atender_cpu(logger, fd);  break;
        case TIPO_MS:  km_atender_ms(logger, fd);   break;
        case TIPO_SWAP: km_atender_swap(logger, fd); break;
    }
    return NULL;
}

void km_atender_ks(t_log *logger, int fd_ks)
{
    log_info(logger, "## Kernel Scheduler Conectado - FD del socket: %d", fd_ks);

    int32_t cod_op;
    while (recibir_int32(fd_ks, &cod_op)) {
        log_info(logger, "KS solicita operación: %d", cod_op);
        
        switch (cod_op) {
            case OP_CREAR_PROCESO: {
                void *stream = NULL;
                int size = 0;
                if (km_recibir_cuerpo_paquete(fd_ks, &stream, &size)) {
                    km_procesar_crear_proceso(fd_ks, stream, size);
                    free(stream);
                }
                break;
            }
            default:
                log_info(logger, "Operación KS desconocida: %d", cod_op);
                break;
        }
    }

    log_warning(logger, "Kernel Scheduler desconectado (fd=%d)", fd_ks);
    close(fd_ks);
}

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
    {
        log_info(logger, "CPU %d solicita op_code: %d", id_cpu, cod_op);
        
        switch (cod_op)
        {
            case OP_FETCH_INSTRUCCION: {
                int32_t pid, pc;
                
                if (!recibir_int32(fd_cpu, &pid)) {
                    log_error(logger, "Error recibiendo PID en FETCH (fd=%d)", fd_cpu);
                    goto cpu_desconectada;
                }
                
                if (!recibir_int32(fd_cpu, &pc)) {
                    log_error(logger, "Error recibiendo PC en FETCH (fd=%d)", fd_cpu);
                    goto cpu_desconectada;
                }

                log_info(logger, "CPU %d solicita FETCH: PID=%d, PC=%d", id_cpu, pid, pc);

                char *instr = km_obtener_instruccion(pid, (int32_t)pc);
                if (!instr) {
                    log_error(logger, "FETCH ERROR: PID=%d PC=%d no encontrado", pid, pc);
                    enviar_int32(fd_cpu, OP_ERROR);
                    break;
                }

                int instruction_delay = config_get_int_value(g_config, "INSTRUCTION_DELAY");
                if (instruction_delay > 0) {
                    log_info(logger, "INSTRUCTION_DELAY: %d ms", instruction_delay);
                    usleep(instruction_delay * 1000);
                }

                log_info(logger, "## PID: %d - Obtener instruccion: %d - Instruccion: %s",
                         pid, pc, instr);

                int32_t tam = (int32_t)strlen(instr);
                
                log_info(logger, "Enviando OK + tamaño=%d + instrucción a CPU %d", tam, id_cpu);
                
                if (!enviar_int32(fd_cpu, OP_OK)) {
                    log_error(logger, "Error enviando OP_OK en FETCH");
                    goto cpu_desconectada;
                }
                
                if (!enviar_int32(fd_cpu, tam)) {
                    log_error(logger, "Error enviando tamaño en FETCH");
                    goto cpu_desconectada;
                }
                
                int sent = send(fd_cpu, instr, tam, MSG_NOSIGNAL);
                if (sent != tam) {
                    log_error(logger, "Error enviando instrucción: esperaba %d, envié %d", tam, sent);
                    goto cpu_desconectada;
                }
                
                log_info(logger, "FETCH completado exitosamente");
                break;
            }
            
            case OP_GET_CONTEXTO: {
                int32_t pid;
                if (!recibir_int32(fd_cpu, &pid)) goto cpu_desconectada;

                log_info(logger, "CPU %d — GET_CONTEXTO PID=%d", id_cpu, pid);

                /* Buscar contexto guardado del proceso */
                t_registros ctx_regs;
                memset(&ctx_regs, 0, sizeof(t_registros));

                pthread_mutex_lock(&mutex_procesos);
                for (int i = 0; i < KM_MAX_PROCESOS; i++) {
                    if (g_procesos[i].activo && g_procesos[i].pid == pid) {
                        memcpy(&ctx_regs, &g_procesos[i].registros, sizeof(t_registros));
                        break;
                    }
                }
                pthread_mutex_unlock(&mutex_procesos);

                log_info(logger, "GET_CONTEXTO: enviando contexto de %zu bytes para PID=%d (PC=%u)",
                        sizeof(t_registros), pid, ctx_regs.PC);

                if (!enviar_int32(fd_cpu, OP_OK)) goto cpu_desconectada;
                if (send(fd_cpu, &ctx_regs, sizeof(t_registros), MSG_NOSIGNAL) != (int)sizeof(t_registros))
                    goto cpu_desconectada;

                log_info(logger, "GET_CONTEXTO completado (%zu bytes enviados)", sizeof(t_registros));
                break;
            }

            case OP_SET_CONTEXTO: {
                int32_t pid;
                if (!recibir_int32(fd_cpu, &pid)) goto cpu_desconectada;

                t_registros ctx_regs;
                if (recv(fd_cpu, &ctx_regs, sizeof(t_registros), MSG_WAITALL) != (int)sizeof(t_registros))
                    goto cpu_desconectada;

                /* Persistir el contexto */
                pthread_mutex_lock(&mutex_procesos);
                for (int i = 0; i < KM_MAX_PROCESOS; i++) {
                    if (g_procesos[i].activo && g_procesos[i].pid == pid) {
                        memcpy(&g_procesos[i].registros, &ctx_regs, sizeof(t_registros));
                        break;
                    }
                }
                pthread_mutex_unlock(&mutex_procesos);

                log_info(logger, "CPU %d — SET_CONTEXTO PID=%d (PC guardado: %u)", id_cpu, pid, ctx_regs.PC);

                if (!enviar_int32(fd_cpu, OP_OK)) goto cpu_desconectada;
                log_info(logger, "SET_CONTEXTO completado");
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

void km_atender_ms(t_log *logger, int fd_ms)
{
    int32_t tamanio;
    if (!recibir_int32(fd_ms, &tamanio)) {
        log_error(logger, "Error recibiendo tamaño de Memory Stick");
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
        log_info(logger, "Memory Stick desconectado");

    __atomic_sub_fetch(&g_ms_conectados, 1, __ATOMIC_SEQ_CST);
    close(fd_ms);
}

void km_atender_swap(t_log *logger, int fd_swap)
{
    int32_t block_size, swap_size;
    if (!recibir_int32(fd_swap, &block_size) ||
        !recibir_int32(fd_swap, &swap_size))
    {
        log_error(logger, "Error recibiendo parámetros de SWAP");
        close(fd_swap);
        return;
    }

    int32_t bloques_totales = (block_size > 0) ? swap_size / block_size : 0;

    __atomic_add_fetch(&g_swap_conectado, 1, __ATOMIC_SEQ_CST);
    log_info(logger, "## Conectado a SWAP (fd=%d) — bloque: %d bytes, total: %d bytes, bloques disponibles: %d",
             fd_swap, block_size, swap_size, bloques_totales);

    if (g_swap_conectado == 1)
        sem_post(&g_sem_listo);

    char probe;
    if (recv(fd_swap, &probe, 1, MSG_WAITALL) <= 0)
        log_info(logger, "SWAP desconectado");

    __atomic_sub_fetch(&g_swap_conectado, 1, __ATOMIC_SEQ_CST);
    close(fd_swap);
}

void *hilo_servidor(void *varg)
{
    t_servidor_arg *arg = (t_servidor_arg *)varg;
    km_iniciar_servidor(arg->logger, arg->puerto);
    free(arg);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <archivo_config>\n", argv[0]);
        return 1;
    }

    g_logger = log_create("kernel_memory.log", "KERNEL_MEMORY", true, LOG_LEVEL_DEBUG);
    if (!g_logger) { fprintf(stderr, "Error al crear logger\n"); return 1; }

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

    inicializar_semaforos();

    int puerto = config_get_int_value(g_config, "PUERTO_ESCUCHA");
    log_info(g_logger, "Kernel Memory iniciando en puerto %d", puerto);

    t_servidor_arg *arg = malloc(sizeof(t_servidor_arg));
    arg->logger = g_logger;
    arg->puerto = puerto;

    pthread_t hilo;
    if (pthread_create(&hilo, NULL, hilo_servidor, arg) != 0) {
        log_error(g_logger, "Error creando hilo servidor");
        return 1;
    }

    pthread_join(hilo, NULL);

    config_destroy(g_config);
    log_destroy(g_logger);
    destruir_semaforos();
    return 0;
}
