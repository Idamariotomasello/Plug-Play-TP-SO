#include "cpu.h"
#include <cliente.h>
#include <server.h>

t_log *logger = NULL;
t_config *config = NULL;
int32_t g_id_cpu = -1;
 
int fd_kernel_scheduler_dispatch = -1;
int fd_kernel_scheduler_interrupt = -1;
int fd_kernel_memory = -1;
int fd_memory_stick = -1;
 
t_interrupcion interrupcion_pendiente = { .activa = false, .motivo = 0 };
pthread_mutex_t mutex_interrupcion_pendiente = PTHREAD_MUTEX_INITIALIZER;

static bool modo_test_sin_memoria = true;
static bool usar_doble_conexion_kernel_scheduler = false;
static bool enviar_syscall_extendida = false;
 
/* =========================================================
 * Helpers de registros
 * ========================================================= */
 
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
 
    enviar_int32(fd_kernel_memory, OP_FETCH_INSTRUCCION);
    enviar_int32(fd_kernel_memory, pid);
    enviar_int32(fd_kernel_memory, (int32_t)pc);
 
    int32_t resp;
    if (!recibir_int32(fd_kernel_memory, &resp) || resp != OP_OK)
    {
        log_error(logger, "FETCH: KM respondio error (PID=%d, PC=%d)", pid, pc);
        return NULL;
    }
 
    int32_t tam;
    if (!recibir_int32(fd_kernel_memory, &tam) || tam <= 0)
        return NULL;
    
    
    if (!enviar_int32(fd_kernel_memory, (int32_t)pc)) {
        log_error(logger, "Error enviando PC en FETCH");
        return NULL;
    }

    log_info(logger, "[FETCH] Esperando respuesta de KM... (OP_OK=%d)", OP_OK);
    
    int32_t resp;
    if (!recibir_int32(fd_kernel_memory, &resp)) {
        log_error(logger, "Error recibiendo respuesta en FETCH (no recibí nada)");
        return NULL;
    }
    
    log_info(logger, "[FETCH] Respuesta recibida: %d (esperaba OP_OK=%d)", resp, OP_OK);
    
    if (resp != OP_OK) {
        log_error(logger, "FETCH: KM respondio error (PID=%d, PC=%d, resp=%d, esperaba %d)", pid, pc, resp, OP_OK);
        return NULL;
    }

    int32_t tam;
    if (!recibir_int32(fd_kernel_memory, &tam)) {
        log_error(logger, "Error recibiendo tamaño de instrucción");
        return NULL;
    }
    
    log_info(logger, "[FETCH] Tamaño recibido: %d bytes", tam);

    if (tam <= 0 || tam > 256) {
        log_error(logger, "Tamaño inválido: %d", tam);
        return NULL;
    }

    char *instruccion = malloc(tam + 1);
    if (recv(fd_kernel_memory, instruccion, tam, MSG_WAITALL) != tam)
    {
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
 
    enviar_int32(fd_memory_stick, OP_LEER_MS);
    enviar_int32(fd_memory_stick, dir_fisica);
    enviar_int32(fd_memory_stick, tamanio);
 
    int32_t resp;
    if (!recibir_int32(fd_memory_stick, &resp) || resp != OP_OK)
        return false;

    int32_t tam_recv;
    if (!recibir_int32(fd_memory_stick, &tam_recv))
        return false;
 
    return recv(fd_memory_stick, dest, tam_recv, MSG_WAITALL) == tam_recv;
}

bool cpu_escribir_memoria(int32_t pid, uint32_t dir_logica,
                                  int32_t tamanio, void *src)
{
    int32_t seg, despl;
    cpu_mmu_traducir(dir_logica, &seg, &despl);
    int32_t dir_fisica = despl;

    log_info(logger, "PID: %d - Acción: ESCRIBIR - Dirección Física: %d",
             pid, dir_fisica);
 
    enviar_int32(fd_memory_stick, OP_ESCRIBIR_MS);
    enviar_int32(fd_memory_stick, dir_fisica);
    enviar_int32(fd_memory_stick, tamanio);
    send(fd_memory_stick, src, tamanio, MSG_NOSIGNAL);
 
    int32_t resp;
    return recibir_int32(fd_memory_stick, &resp) && resp == OP_OK;
}

void cpu_limpiar_syscall(t_syscall_pendiente *syscall)
{
    if (!syscall)
        return;

    memset(syscall, 0, sizeof(t_syscall_pendiente));
    syscall->tipo = SYSCALL_NINGUNA;
}

static void cpu_cargar_syscall(t_syscall_pendiente *syscall,
                               e_tipo_syscall tipo,
                               const char *nombre,
                               const char *parametro_1,
                               const char *parametro_2,
                               uint32_t valor_1,
                               uint32_t valor_2)
{
    if (!syscall)
        return;

    cpu_limpiar_syscall(syscall);
    syscall->tipo = tipo;
    snprintf(syscall->nombre, sizeof(syscall->nombre), "%s", nombre ? nombre : "");
    snprintf(syscall->parametro_1, sizeof(syscall->parametro_1), "%s", parametro_1 ? parametro_1 : "");
    snprintf(syscall->parametro_2, sizeof(syscall->parametro_2), "%s", parametro_2 ? parametro_2 : "");
    syscall->valor_1 = valor_1;
    syscall->valor_2 = valor_2;
}

static void cpu_avanzar_pc_si_corresponde(t_contexto *ctx, bool modifico_pc)
{
    if (!modifico_pc)
        ctx->regs.PC++;
}

static bool cpu_enviar_string(int fd, const char *texto)
{
    int32_t largo = texto ? (int32_t)strlen(texto) : 0;
    enviar_int32(fd, largo);

    if (largo <= 0)
        return true;

    return send(fd, texto, largo, MSG_NOSIGNAL) == largo;
}

bool cpu_conectar_kernel_scheduler(const char *ip, int puerto)
{
    log_info(logger, "Conectando a Kernel Scheduler %s:%d...", ip, puerto);
    fd_kernel_scheduler_dispatch = conectar_a_servidor(logger, ip, puerto);
    if (fd_kernel_scheduler_dispatch == -1)
        return false;

    if (!enviar_handshake(logger, fd_kernel_scheduler_dispatch, TIPO_CPU))
        return false;

    enviar_int32(fd_kernel_scheduler_dispatch, g_id_cpu);

    if (usar_doble_conexion_kernel_scheduler) {
        enviar_int32(fd_kernel_scheduler_dispatch, CANAL_CPU_DISPATCH);

        fd_kernel_scheduler_interrupt = conectar_a_servidor(logger, ip, puerto);
        if (fd_kernel_scheduler_interrupt == -1)
            return false;

        if (!enviar_handshake(logger, fd_kernel_scheduler_interrupt, TIPO_CPU))
            return false;

        enviar_int32(fd_kernel_scheduler_interrupt, g_id_cpu);
        enviar_int32(fd_kernel_scheduler_interrupt, CANAL_CPU_INTERRUPT);
    }

    return true;
}

bool cpu_enviar_retorno_kernel_scheduler(int32_t pid, e_motivo_retorno motivo, const t_syscall_pendiente *syscall)
{
    enviar_int32(fd_kernel_scheduler_dispatch, pid);
    enviar_int32(fd_kernel_scheduler_dispatch, (int32_t)motivo);

    if (motivo != MOTIVO_SYSCALL || !enviar_syscall_extendida || syscall == NULL)
        return true;

    enviar_int32(fd_kernel_scheduler_dispatch, (int32_t)syscall->tipo);
    if (!cpu_enviar_string(fd_kernel_scheduler_dispatch, syscall->nombre))
        return false;
    if (!cpu_enviar_string(fd_kernel_scheduler_dispatch, syscall->parametro_1))
        return false;
    if (!cpu_enviar_string(fd_kernel_scheduler_dispatch, syscall->parametro_2))
        return false;
    enviar_int32(fd_kernel_scheduler_dispatch, (int32_t)syscall->valor_1);
    enviar_int32(fd_kernel_scheduler_dispatch, (int32_t)syscall->valor_2);

    return true;
}

bool cpu_execute(t_contexto *ctx, const char *linea,
                         e_motivo_retorno *motivo, t_syscall_pendiente *syscall)
{
    char op[32], a1[32], a2[32];
    op[0] = a1[0] = a2[0] = '\0';
    int campos = sscanf(linea, "%31s %31s %31s", op, a1, a2);
    bool modifico_pc = false;
    cpu_limpiar_syscall(syscall);
 
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
        uint32_t buf = 0;
        if (modo_test_sin_memoria) {
            log_info(logger, "PID: %d - MOV_IN en modo test sin memoria; se usa valor 0", ctx->pid);
        } else {
            /* Codigo real para entrega con memoria completa. */
            cpu_leer_memoria(ctx->pid, ctx->regs.SI, sizeof(uint32_t), &buf);
        }
        cpu_escribir_registro(&ctx->regs, a1, buf);
        log_info(logger, "PID: %d - Acción: LEER - Dirección Física: %d - Valor: %u",
                 ctx->pid, ctx->regs.SI, buf);
    }
    else if (!strcmp(op, "MOV_OUT")) {
        uint32_t val = cpu_leer_registro(&ctx->regs, a1);
        if (modo_test_sin_memoria) {
            log_info(logger, "PID: %d - MOV_OUT en modo test sin memoria; no se escribe Memory Stick", ctx->pid);
        } else {
            /* Codigo real para entrega con memoria completa. */
            cpu_escribir_memoria(ctx->pid, ctx->regs.DI, sizeof(uint32_t), &val);
        }
        log_info(logger, "PID: %d - Acción: ESCRIBIR - Dirección Física: %d - Valor: %u",
                 ctx->pid, ctx->regs.DI, val);
    }
    else if (!strcmp(op, "COPY_MEM")) {
        int32_t tam = (int32_t)cpu_leer_registro(&ctx->regs, a1);
        if (modo_test_sin_memoria) {
            log_info(logger, "PID: %d - COPY_MEM en modo test sin memoria; tamanio: %d", ctx->pid, tam);
        } else {
            /* Codigo real para entrega con memoria completa. */
            void *buf = malloc(tam);
            cpu_leer_memoria(ctx->pid, ctx->regs.SI, tam, buf);
            cpu_escribir_memoria(ctx->pid, ctx->regs.DI, tam, buf);
            free(buf);
        }
    }
    /* Syscalls */
    else if (!strcmp(op, "EXIT")) {
        log_info(logger, "## PID: %d - Syscall: EXIT", ctx->pid);
        *motivo = MOTIVO_EXIT;
        return false;
    }
    else if (!strcmp(op, "SLEEP")) {
        cpu_cargar_syscall(syscall, SYSCALL_SLEEP, op, a1, "", (uint32_t)atoi(a1), 0);
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else if (!strcmp(op, "STDIN")) {
        cpu_cargar_syscall(syscall, SYSCALL_STDIN, op, a1, a2,
                           cpu_leer_registro(&ctx->regs, a1),
                           cpu_leer_registro(&ctx->regs, a2));
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else if (!strcmp(op, "STDOUT")) {
        cpu_cargar_syscall(syscall, SYSCALL_STDOUT, op, a1, a2,
                           cpu_leer_registro(&ctx->regs, a1),
                           cpu_leer_registro(&ctx->regs, a2));
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else if (!strcmp(op, "MUTEX_CREATE")) {
        cpu_cargar_syscall(syscall, SYSCALL_MUTEX_CREATE, op, a1, "", 0, 0);
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else if (!strcmp(op, "MUTEX_LOCK")) {
        cpu_cargar_syscall(syscall, SYSCALL_MUTEX_LOCK, op, a1, "", 0, 0);
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else if (!strcmp(op, "MUTEX_UNLOCK")) {
        cpu_cargar_syscall(syscall, SYSCALL_MUTEX_UNLOCK, op, a1, "", 0, 0);
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else if (!strcmp(op, "MEM_ALLOC")) {
        cpu_cargar_syscall(syscall, SYSCALL_MEM_ALLOC, op, a1, a2,
                           (uint32_t)atoi(a1), (uint32_t)atoi(a2));
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else if (!strcmp(op, "MEM_FREE")) {
        cpu_cargar_syscall(syscall, SYSCALL_MEM_FREE, op, a1, "", (uint32_t)atoi(a1), 0);
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else if (!strcmp(op, "INIT_PROC")) {
        cpu_cargar_syscall(syscall, SYSCALL_INIT_PROC, op, a1, a2, 0, (uint32_t)atoi(a2));
        cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else {
        log_error(logger, "Instrucción desconocida: %s", op);
        *motivo = MOTIVO_ERROR;
        return false;
    }
 
    cpu_avanzar_pc_si_corresponde(ctx, modifico_pc);
 
    return true;
}
 
/* =========================================================
 * hilo_interrupciones
 * Escucha interrupciones asíncronas del KS sobre fd_kernel_scheduler_interrupt.
 * Las almacena en interrupcion_pendiente protegido por mutex_interrupcion_pendiente.
 * ========================================================= */
 
void *hilo_interrupciones(void *arg)
{
    (void)arg;
    int32_t motivo;
    int fd_interrupciones = usar_doble_conexion_kernel_scheduler
        ? fd_kernel_scheduler_interrupt
        : fd_kernel_scheduler_dispatch;

    while (recibir_int32(fd_interrupciones, &motivo))
    {
        log_info(logger, "## Interrupción recibida");
        pthread_mutex_lock(&mutex_interrupcion_pendiente);
        interrupcion_pendiente.activa = true;
        interrupcion_pendiente.motivo = motivo;
        pthread_mutex_unlock(&mutex_interrupcion_pendiente);
    }
    return NULL;
}

void cpu_ciclo_instruccion(t_contexto *ctx)
{
    e_motivo_retorno motivo = MOTIVO_EXIT;
    t_syscall_pendiente syscall;
    cpu_limpiar_syscall(&syscall);
 
    while (1)
    {
        /* ── FETCH ── */
        char *instruccion = cpu_fetch(ctx->pid, ctx->regs.PC);
        if (!instruccion) {
            log_error(logger, "FETCH fallo (PID=%d, PC=%d)", ctx->pid, ctx->regs.PC);
            motivo = MOTIVO_ERROR;
            break;
        }
 
        /* ── EXECUTE ── */
        bool continuar = cpu_execute(ctx, instruccion, &motivo, &syscall);
        free(instruccion);
 
        if (!continuar)
            break;
 
        /* ── CHECK INTERRUPT ── */
        pthread_mutex_lock(&mutex_interrupcion_pendiente);
        bool hay_irq = interrupcion_pendiente.activa;
        int32_t irq_motivo = interrupcion_pendiente.motivo;
        pthread_mutex_unlock(&mutex_interrupcion_pendiente);
 
        if (hay_irq)
        {
            log_info(logger, "## Interrupción recibida");
            motivo = (e_motivo_retorno)irq_motivo;
 
            pthread_mutex_lock(&mutex_interrupcion_pendiente);
            interrupcion_pendiente.activa = false;
            pthread_mutex_unlock(&mutex_interrupcion_pendiente);
            break;
        }
        pthread_mutex_unlock(&mutex_irq);
    }
 
    /* ── Guardar contexto en KM y devolver PID al KS ── */
    enviar_int32(fd_kernel_memory, OP_SET_CONTEXTO);
    enviar_int32(fd_kernel_memory, ctx->pid);
    send(fd_kernel_memory, &ctx->regs, sizeof(t_registros), MSG_NOSIGNAL);
 
    int32_t ack;
    recibir_int32(fd_kernel_memory, &ack);
 
    /* Devolver PID al KS con motivo */
    cpu_enviar_retorno_kernel_scheduler(ctx->pid, motivo, &syscall);

    if (motivo == MOTIVO_SYSCALL) {
        log_info(logger, "## PID: %d - Syscall pendiente: %s - %s %s - valores: %u %u",
                 ctx->pid,
                 syscall.nombre,
                 syscall.parametro_1,
                 syscall.parametro_2,
                 syscall.valor_1,
                 syscall.valor_2);
        /*
         * TODO entrega 2 integrada con Kernel Scheduler:
         * Si ENVIAR_SYSCALL_EXTENDIDA=1, tambien se serializa por el socket
         * de dispatch para que Kernel Scheduler pueda resolver la syscall.
         */
    }
 
    log_info(logger, "## PID: %d devuelto al KS — motivo: %d", ctx->pid, motivo);
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

    if (config_has_property(config, "MODO_TEST_SIN_MEMORIA"))
        modo_test_sin_memoria = config_get_int_value(config, "MODO_TEST_SIN_MEMORIA") != 0;
    if (config_has_property(config, "USAR_DOBLE_CONEXION_KS"))
        usar_doble_conexion_kernel_scheduler = config_get_int_value(config, "USAR_DOBLE_CONEXION_KS") != 0;
    if (config_has_property(config, "ENVIAR_SYSCALL_EXTENDIDA"))
        enviar_syscall_extendida = config_get_int_value(config, "ENVIAR_SYSCALL_EXTENDIDA") != 0;
    if (enviar_syscall_extendida)
        log_info(logger, "CPU con protocolo extendido de syscall habilitado");
 
    /* ── 1. Conectar a Kernel Scheduler ── */
    char *ip_ks     = config_get_string_value(config, "IP_KERNEL_SCHEDULER");
    int   puerto_ks = config_get_int_value(config, "PUERTO_KERNEL_SCHEDULER");
 
    if (!cpu_conectar_kernel_scheduler(ip_ks, puerto_ks))
    {
        free(ip_ks);
        log_error(logger, "Fallo conexion/protocolo con KS");
        goto cleanup;
    }
    log_info(logger, "## CPU %d conectada al Kernel Scheduler", g_id_cpu);
    if (usar_doble_conexion_kernel_scheduler)
        log_info(logger, "## CPU %d conectada al canal de interrupciones del Kernel Scheduler", g_id_cpu);
    free(ip_ks);
 
    /* ── 2. Conectar a Kernel Memory ── */
    char *ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");

    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_kernel_memory = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
    if (fd_kernel_memory == -1) { log_error(logger, "Fallo conexion KM"); goto cleanup; }
 
    if (!enviar_handshake(logger, fd_kernel_memory, TIPO_CPU))
    { log_error(logger, "Handshake fallo con KM"); goto cleanup; }
 
    enviar_int32(fd_kernel_memory, g_id_cpu);
    log_info(logger, "## CPU %d conectada al Kernel Memory", g_id_cpu);

    /* Conectar a Memory Stick */
    char *ip_ms = config_get_string_value(config, "IP_KERNEL_MEMORY_STICK");
    int   puerto_ms = config_get_int_value(config, "PUERTO_MEMORY_STICK");

    log_info(logger, "Conectando a Memory Stick %s:%d...", ip_ms, puerto_ms);
    fd_memory_stick = conectar_a_servidor(logger, ip_ms, puerto_ms);
    free(ip_ms);
    if (fd_memory_stick == -1)
    {
        log_warning(logger, "No se pudo conectar al Memory Stick — operaciones de memoria no disponibles");
        /* No es fatal en checkpoint 1 */
    }
    else
        log_info(logger, "## CPU %d conectada al Memory Stick", g_id_cpu);
 
    pthread_t hilo_kernel_scheduler_interrupt;
    if (usar_doble_conexion_kernel_scheduler)
    {
        if (pthread_create(&hilo_kernel_scheduler_interrupt, NULL, hilo_interrupciones, NULL) != 0)
        {
            log_error(logger, "No se pudo crear el hilo de interrupciones");
            goto cleanup;
        }
        pthread_detach(hilo_kernel_scheduler_interrupt);
    }
    else
    {
        log_warning(logger, "CPU iniciada sin canal separado de interrupciones");
    }
 
    log_info(logger, "CPU %d lista — esperando PID del Kernel Scheduler", g_id_cpu);

    /* Loop principal: esperar PID, ejecutar, devolver */
    int32_t pid;
    while (recibir_int32(fd_kernel_scheduler_dispatch, &pid))
    {
        log_info(logger, "## CPU %d recibio PID %d — solicitando contexto", g_id_cpu, pid);
 
        /* Pedir contexto al KM */
        enviar_int32(fd_kernel_memory, OP_GET_CONTEXTO);
        enviar_int32(fd_kernel_memory, pid);
 
        int32_t resp;
        if (!recibir_int32(fd_kernel_memory, &resp) || resp != OP_OK)
        {
            log_error(logger, "No se pudo obtener contexto de PID %d", pid);
            enviar_int32(fd_kernel_scheduler_dispatch, pid);
            enviar_int32(fd_kernel_scheduler_dispatch, (int32_t)MOTIVO_SEG_FAULT);
            continue;
        }

        t_contexto ctx;
        ctx.pid = pid;
        if (recv(fd_kernel_memory, &ctx.regs, sizeof(t_registros), MSG_WAITALL)
            != sizeof(t_registros))
        {
            log_error(logger, "Error recibiendo contexto de PID %d", pid);
            continue;
        }

        log_info(logger, "## PID: %d — Contexto cargado (PC=%u)", pid, ctx.regs.PC);

        /* Ejecutar ciclo de instrucción */
        cpu_ciclo_instruccion(&ctx);
    }

    log_info(logger, "CPU %d — KS desconectado, finalizando", g_id_cpu);

cleanup:
    if (fd_kernel_scheduler_dispatch > 0) close(fd_kernel_scheduler_dispatch);
    if (fd_kernel_scheduler_interrupt > 0) close(fd_kernel_scheduler_interrupt);
    if (fd_kernel_memory > 0) close(fd_kernel_memory);
    if (fd_memory_stick > 0) close(fd_memory_stick);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}
