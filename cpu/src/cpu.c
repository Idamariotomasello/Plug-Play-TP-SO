#include "cpu.h"
#include <cliente.h>
#include <server.h>

t_log *logger = NULL;
t_config *config = NULL;
int32_t g_id_cpu = -1;

int fd_ks = -1;
int fd_km = -1;
int fd_ms = -1;

t_interrupcion  g_irq = { .activa = false, .motivo = 0 };
pthread_mutex_t mutex_irq = PTHREAD_MUTEX_INITIALIZER;

const char *cpu_nombre_motivo(e_motivo_retorno m)
{
    switch (m) {
        case MOTIVO_NINGUNO:      return "NINGUNO";
        case MOTIVO_SYSCALL:      return "SYSCALL";
        case MOTIVO_INTERRUPCION: return "INTERRUPCION";
        case MOTIVO_ERROR:        return "ERROR";
        case MOTIVO_EXIT:         return "EXIT";
        default:                  return "DESCONOCIDO";
    }
}

uint32_t cpu_leer_registro(t_registros *r, const char *n)
{
    if (!strcmp(n,"PC"))  return r->PC;
    if (!strcmp(n,"AX"))  return r->AX;
    if (!strcmp(n,"BX"))  return r->BX;
    if (!strcmp(n,"CX"))  return r->CX;
    if (!strcmp(n,"DX"))  return r->DX;
    if (!strcmp(n,"EAX")) return r->EAX;
    if (!strcmp(n,"EBX")) return r->EBX;
    if (!strcmp(n,"ECX")) return r->ECX;
    if (!strcmp(n,"EDX")) return r->EDX;
    if (!strcmp(n,"SI"))  return r->SI;
    if (!strcmp(n,"DI"))  return r->DI;
    log_warning(logger, "Registro desconocido: %s", n);
    return 0;
}

void cpu_escribir_registro(t_registros *r, const char *n, uint32_t v)
{
    if (!strcmp(n,"PC"))  { r->PC  = v; return; }
    if (!strcmp(n,"AX"))  { r->AX  = (uint8_t)v; return; }
    if (!strcmp(n,"BX"))  { r->BX  = (uint8_t)v; return; }
    if (!strcmp(n,"CX"))  { r->CX  = (uint8_t)v; return; }
    if (!strcmp(n,"DX"))  { r->DX  = (uint8_t)v; return; }
    if (!strcmp(n,"EAX")) { r->EAX = v; return; }
    if (!strcmp(n,"EBX")) { r->EBX = v; return; }
    if (!strcmp(n,"ECX")) { r->ECX = v; return; }
    if (!strcmp(n,"EDX")) { r->EDX = v; return; }
    if (!strcmp(n,"SI"))  { r->SI  = v; return; }
    if (!strcmp(n,"DI"))  { r->DI  = v; return; }
    log_warning(logger, "Registro desconocido al escribir: %s", n);
}

char *cpu_fetch(int32_t pid, uint32_t pc)
{
    log_info(logger, "## PID: %d - FETCH - Program Counter: %d", pid, pc);
    log_info(logger, "[FETCH] Enviando solicitud a KM: OP_FETCH_INSTRUCCION(%d)", OP_FETCH_INSTRUCCION);

    if (!enviar_int32(fd_km, OP_FETCH_INSTRUCCION)) {
        log_error(logger, "Error enviando OP_FETCH_INSTRUCCION");
        return NULL;
    }
    
    if (!enviar_int32(fd_km, pid)) {
        log_error(logger, "Error enviando PID en FETCH");
        return NULL;
    }
    
    if (!enviar_int32(fd_km, (int32_t)pc)) {
        log_error(logger, "Error enviando PC en FETCH");
        return NULL;
    }

    log_info(logger, "[FETCH] Esperando respuesta de KM... (OP_OK=%d)", OP_OK);
    
    int32_t resp;
    if (!recibir_int32(fd_km, &resp)) {
        log_error(logger, "Error recibiendo respuesta en FETCH (no recibí nada)");
        return NULL;
    }
    
    log_info(logger, "[FETCH] Respuesta recibida: %d (esperaba OP_OK=%d)", resp, OP_OK);
    
    if (resp != OP_OK) {
        log_error(logger, "FETCH: KM respondio error (PID=%d, PC=%d, resp=%d, esperaba %d)", pid, pc, resp, OP_OK);
        return NULL;
    }

    int32_t tam;
    if (!recibir_int32(fd_km, &tam)) {
        log_error(logger, "Error recibiendo tamaño de instrucción");
        return NULL;
    }
    
    log_info(logger, "[FETCH] Tamaño recibido: %d bytes", tam);

    if (tam <= 0 || tam > 256) {
        log_error(logger, "Tamaño inválido: %d", tam);
        return NULL;
    }

    char *instruccion = malloc(tam + 1);
    if (!instruccion) {
        log_error(logger, "Error malloc instrucción");
        return NULL;
    }
    
    int recibido = recv(fd_km, instruccion, tam, MSG_WAITALL);
    if (recibido != tam) {
        log_error(logger, "Error recibiendo instrucción: esperaba %d, recibí %d", tam, recibido);
        free(instruccion);
        return NULL;
    }
    
    instruccion[tam] = '\0';
    log_info(logger, "[FETCH] Instrucción recibida: '%s'", instruccion);
    
    return instruccion;
}

int32_t g_segment_max_size = 256;

void cpu_mmu_traducir(uint32_t dir_logica,
                              int32_t *num_seg, int32_t *despl)
{
    *num_seg = (int32_t)floor((double)dir_logica / g_segment_max_size);
    *despl   = (int32_t)(dir_logica % (uint32_t)g_segment_max_size);
}

bool cpu_leer_memoria(int32_t pid, uint32_t dir_logica,
                              int32_t tamanio, void *dest)
{
    int32_t seg, despl;
    cpu_mmu_traducir(dir_logica, &seg, &despl);
    int32_t dir_fisica = despl;

    log_info(logger, "PID: %d - Acción: LEER - Dirección Física: %d",
             pid, dir_fisica);

    enviar_int32(fd_ms, OP_LEER_MS);
    enviar_int32(fd_ms, dir_fisica);
    enviar_int32(fd_ms, tamanio);

    int32_t resp;
    if (!recibir_int32(fd_ms, &resp) || resp != OP_OK)
        return false;

    int32_t tam_recv;
    if (!recibir_int32(fd_ms, &tam_recv))
        return false;

    return recv(fd_ms, dest, tam_recv, MSG_WAITALL) == tam_recv;
}

bool cpu_escribir_memoria(int32_t pid, uint32_t dir_logica,
                                  int32_t tamanio, void *src)
{
    int32_t seg, despl;
    cpu_mmu_traducir(dir_logica, &seg, &despl);
    int32_t dir_fisica = despl;

    log_info(logger, "PID: %d - Acción: ESCRIBIR - Dirección Física: %d",
             pid, dir_fisica);

    enviar_int32(fd_ms, OP_ESCRIBIR_MS);
    enviar_int32(fd_ms, dir_fisica);
    enviar_int32(fd_ms, tamanio);
    send(fd_ms, src, tamanio, MSG_NOSIGNAL);

    int32_t resp;
    return recibir_int32(fd_ms, &resp) && resp == OP_OK;
}

bool cpu_execute(t_contexto *ctx, const char *linea,
                         e_motivo_retorno *motivo)
{
    char op[32], a1[32], a2[32];
    op[0] = a1[0] = a2[0] = '\0';
    int campos = sscanf(linea, "%31s %31s %31s", op, a1, a2);
    bool modifico_pc = false;

    log_info(logger, "## PID: %d - Ejecutando: %s%s%s%s%s",
             ctx->pid, op,
             campos > 1 ? " " : "", a1,
             campos > 2 ? " " : "", a2);

    /* Instrucciones de registro */
    if (!strcmp(op, "NOOP")) {
        log_info(logger, "[EXECUTE] NOOP - sin operación");
    }
    else if (!strcmp(op, "SET")) {
        if (campos < 3) {
            log_error(logger, "SET requiere 2 argumentos");
            *motivo = MOTIVO_ERROR;
            return false;
        }
        uint32_t val = (uint32_t)atoi(a2);
        cpu_escribir_registro(&ctx->regs, a1, val);
        log_info(logger, "[EXECUTE] SET %s = %u", a1, val);
    }
    else if (!strcmp(op, "SUM")) {
        if (campos < 3) {
            log_error(logger, "SUM requiere 2 argumentos");
            *motivo = MOTIVO_ERROR;
            return false;
        }
        uint32_t dest_val = cpu_leer_registro(&ctx->regs, a1);
        uint32_t orig_val = cpu_leer_registro(&ctx->regs, a2);
        uint32_t resultado = dest_val + orig_val;
        cpu_escribir_registro(&ctx->regs, a1, resultado);
        log_info(logger, "[EXECUTE] SUM %s(%u) + %s(%u) = %u", a1, dest_val, a2, orig_val, resultado);
    }
    else if (!strcmp(op, "SUB")) {
        if (campos < 3) {
            log_error(logger, "SUB requiere 2 argumentos");
            *motivo = MOTIVO_ERROR;
            return false;
        }
        uint32_t dest_val = cpu_leer_registro(&ctx->regs, a1);
        uint32_t orig_val = cpu_leer_registro(&ctx->regs, a2);
        uint32_t resultado = dest_val - orig_val;
        cpu_escribir_registro(&ctx->regs, a1, resultado);
        log_info(logger, "[EXECUTE] SUB %s(%u) - %s(%u) = %u", a1, dest_val, a2, orig_val, resultado);
    }
    else if (!strcmp(op, "JNZ")) {
        if (campos < 3) {
            log_error(logger, "JNZ requiere 2 argumentos");
            *motivo = MOTIVO_ERROR;
            return false;
        }
        uint32_t reg_val = cpu_leer_registro(&ctx->regs, a1);
        if (reg_val != 0) {
            uint32_t new_pc = (uint32_t)atoi(a2);
            log_info(logger, "[EXECUTE] JNZ %s(%u) != 0, saltando a PC=%u", a1, reg_val, new_pc);
            cpu_escribir_registro(&ctx->regs, "PC", new_pc);
            modifico_pc = true;
        } else {
            log_info(logger, "[EXECUTE] JNZ %s(%u) == 0, sin salto", a1, reg_val);
        }
    }
    /* Instrucciones de memoria */
    else if (!strcmp(op, "MOV_IN")) {
        if (campos < 2) {
            log_error(logger, "MOV_IN requiere 1 argumento");
            *motivo = MOTIVO_ERROR;
            return false;
        }
        log_info(logger, "[EXECUTE] MOV_IN %s - leyendo de memoria", a1);
        /* TODO: Implementar lectura de memoria */
    }
    else if (!strcmp(op, "MOV_OUT")) {
        if (campos < 2) {
            log_error(logger, "MOV_OUT requiere 1 argumento");
            *motivo = MOTIVO_ERROR;
            return false;
        }
        log_info(logger, "[EXECUTE] MOV_OUT %s - escribiendo a memoria", a1);
        /* TODO: Implementar escritura de memoria */
    }
    else if (!strcmp(op, "COPY_MEM")) {
        if (campos < 2) {
            log_error(logger, "COPY_MEM requiere 1 argumento");
            *motivo = MOTIVO_ERROR;
            return false;
        }
        log_info(logger, "[EXECUTE] COPY_MEM %s", a1);
        /* TODO: Implementar copia de memoria */
    }
    /* Syscalls */
    else if (!strcmp(op, "EXIT")) {
        log_info(logger, "## PID: %d - Syscall: EXIT", ctx->pid);
        *motivo = MOTIVO_EXIT;
        return false;
    }
    else if (!strcmp(op, "SLEEP") || !strcmp(op, "STDIN") || !strcmp(op, "STDOUT") ||
             !strcmp(op, "MUTEX_CREATE") || !strcmp(op, "MUTEX_LOCK") || !strcmp(op, "MUTEX_UNLOCK") ||
             !strcmp(op, "MEM_ALLOC") || !strcmp(op, "MEM_FREE") || !strcmp(op, "INIT_PROC")) {
        log_info(logger, "## PID: %d - Syscall: %s", ctx->pid, op);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else {
        log_error(logger, "Instrucción desconocida: %s", op);
        *motivo = MOTIVO_ERROR;
        return false;
    }

    if (!modifico_pc) {
        uint32_t new_pc = cpu_leer_registro(&ctx->regs, "PC") + 1;
        cpu_escribir_registro(&ctx->regs, "PC", new_pc);
        log_info(logger, "[EXECUTE] PC incrementado a %u", new_pc);
    }

    return true;
}

void *hilo_interrupciones(void *arg)
{
    (void)arg;
    int32_t motivo;
    while (recibir_int32(fd_ks, &motivo)) {
        pthread_mutex_lock(&mutex_irq);
        g_irq.activa = true;
        g_irq.motivo = motivo;
        pthread_mutex_unlock(&mutex_irq);
        log_info(logger, "## Interrupción recibida: motivo=%d", motivo);
    }
    return NULL;
}

void cpu_ciclo_instruccion(t_contexto *ctx)
{
    e_motivo_retorno motivo = MOTIVO_EXIT;

    while (1) {
        log_info(logger, "[CICLO] Inicio ciclo PC=%u", ctx->regs.PC);
        
        /* FETCH */
        char *instruccion = cpu_fetch(ctx->pid, ctx->regs.PC);
        if (!instruccion) {
            log_error(logger, "FETCH fallo (PID=%d, PC=%d)", ctx->pid, ctx->regs.PC);
            motivo = MOTIVO_ERROR;
            break;
        }

        /* DECODE & EXECUTE */
        if (!cpu_execute(ctx, instruccion, &motivo)) {
            log_info(logger, "[CICLO] Instrucción causó fin de ciclo, motivo=%d", motivo);
            free(instruccion);
            break;
        }

        free(instruccion);

        /* CHECK INTERRUPT */
        pthread_mutex_lock(&mutex_irq);
        if (g_irq.activa) {
            log_info(logger, "## Interrupción recibida");
            motivo = (e_motivo_retorno)g_irq.motivo;
            g_irq.activa = false;
            pthread_mutex_unlock(&mutex_irq);
            break;
        }
        pthread_mutex_unlock(&mutex_irq);
    }

    /* Guardar contexto en KM y devolver PID al KS */
    log_info(logger, "[CICLO] Guardando contexto en KM...");
    
    enviar_int32(fd_km, OP_SET_CONTEXTO);
    enviar_int32(fd_km, ctx->pid);
    send(fd_km, &ctx->regs, sizeof(t_registros), MSG_NOSIGNAL);

    int32_t ack;
    recibir_int32(fd_km, &ack);

    /* Devolver PID al KS con motivo */
    log_info(logger, "[CICLO] Enviando PID al KS con motivo=%d (%s)", motivo, cpu_nombre_motivo(motivo));
    
    enviar_int32(fd_ks, ctx->pid);
    enviar_int32(fd_ks, (int32_t)motivo);

    log_info(logger, "## PID: %d devuelto al KS — motivo: %d (%s)", ctx->pid, motivo, cpu_nombre_motivo(motivo));
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <config> <id_cpu>\n", argv[0]);
        return 1;
    }

    g_id_cpu = atoi(argv[2]);

    char log_name[64];
    snprintf(log_name, sizeof(log_name), "cpu%d.log", g_id_cpu);
    logger = log_create(log_name, "CPU", true, LOG_LEVEL_DEBUG);
    if (!logger) {
        fprintf(stderr, "Error al crear logger\n");
        return 1;
    }

    config = config_create(argv[1]);
    if (!config) {
        char alt[512];
        snprintf(alt, sizeof(alt), "../%s", argv[1]);
        config = config_create(alt);
    }
    if (!config) {
        log_error(logger, "No se pudo cargar '%s'", argv[1]);
        log_destroy(logger); return 1;
    }

    log_info(logger, "CPU %d iniciando...", g_id_cpu);

    if (config_has_property(config, "SEGMENT_MAX_SIZE"))
        g_segment_max_size = config_get_int_value(config, "SEGMENT_MAX_SIZE");

    /* Conectar a Kernel Scheduler */
    char *ip_ks = config_get_string_value(config, "IP_KERNEL_SCHEDULER");
    int   puerto_ks = config_get_int_value(config, "PUERTO_KERNEL_SCHEDULER");

    log_info(logger, "Conectando a Kernel Scheduler %s:%d...", ip_ks, puerto_ks);
    fd_ks = conectar_a_servidor(logger, ip_ks, puerto_ks);
    free(ip_ks);
    
    if (fd_ks == -1) {
        log_error(logger, "Fallo conexion KS");
        goto cleanup;
    }

    if (!enviar_handshake(logger, fd_ks, TIPO_CPU)) {
        log_error(logger, "Handshake fallo con KS");
        goto cleanup;
    }

    enviar_int32(fd_ks, g_id_cpu);
    log_info(logger, "## CPU %d conectada al Kernel Scheduler", g_id_cpu);

    /* Conectar a Kernel Memory */
    char *ip_km = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");

    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_km = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
    
    if (fd_km == -1) {
        log_error(logger, "Fallo conexion KM");
        goto cleanup;
    }

    if (!enviar_handshake(logger, fd_km, TIPO_CPU)) {
        log_error(logger, "Handshake fallo con KM");
        goto cleanup;
    }

    enviar_int32(fd_km, g_id_cpu);
    log_info(logger, "## CPU %d conectada al Kernel Memory", g_id_cpu);

    /* Conectar a Memory Stick */
    char *ip_ms = config_get_string_value(config, "IP_KERNEL_MEMORY_STICK");
    int   puerto_ms = config_get_int_value(config, "PUERTO_MEMORY_STICK");

    log_info(logger, "Conectando a Memory Stick %s:%d...", ip_ms, puerto_ms);
    fd_ms = conectar_a_servidor(logger, ip_ms, puerto_ms);
    free(ip_ms);
    
    if (fd_ms == -1) {
        log_error(logger, "Fallo conexion MS");
        goto cleanup;
    }
    else
        log_info(logger, "## CPU %d conectada al Memory Stick", g_id_cpu);

    /* Hilo de interrupciones */
    pthread_t hilo_irq;
    pthread_create(&hilo_irq, NULL, hilo_interrupciones, NULL);

    log_info(logger, "CPU %d lista — esperando PID del Kernel Scheduler", g_id_cpu);

    /* Loop principal: esperar PID, ejecutar, devolver */
    int32_t pid;
    while (recibir_int32(fd_ks, &pid)) {
        log_info(logger, "## CPU %d recibio PID %d — solicitando contexto", g_id_cpu, pid);

        /* Solicitar contexto a KM */
        enviar_int32(fd_km, OP_GET_CONTEXTO);
        enviar_int32(fd_km, pid);

        int32_t resp;
        if (!recibir_int32(fd_km, &resp) || resp != OP_OK) {
            log_error(logger, "Error solicitando contexto para PID %d", pid);
            continue;
        }

        t_contexto ctx;
        ctx.pid = pid;
        
        int recv_size = recv(fd_km, &ctx.regs, sizeof(t_registros), MSG_WAITALL);
        if (recv_size != sizeof(t_registros)) {
            log_error(logger, "Error recibiendo contexto (recibí %d bytes)", recv_size);
            continue;
        }

        log_info(logger, "## PID: %d — Contexto cargado (PC=%u)", pid, ctx.regs.PC);

        /* Ejecutar ciclo de instrucción */
        cpu_ciclo_instruccion(&ctx);
    }

    log_info(logger, "CPU %d — KS desconectado, finalizando", g_id_cpu);

cleanup:
    if (fd_ks > 0) close(fd_ks);
    if (fd_km > 0) close(fd_km);
    if (fd_ms > 0) close(fd_ms);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}
