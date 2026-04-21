#include "cpu.h"
#include <cliente.h>
#include <server.h>


t_log *logger   = NULL;
t_config *config   = NULL;
int32_t g_id_cpu = -1;
 
int fd_ks = -1;
int fd_km = -1;
int fd_ms = -1;
 
t_interrupcion  g_irq = { .activa = false, .motivo = 0 };
pthread_mutex_t mutex_irq = PTHREAD_MUTEX_INITIALIZER;
 
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
 
/* =========================================================
 * FETCH — pide instrucción al KM usando PC actual
 * Protocolo: OP_FETCH_INSTRUCCION | PID | PC
 * KM responde: OP_OK | tamanio_string | string_instruccion
 * ========================================================= */
 
char *cpu_fetch(int32_t pid, uint32_t pc)
{
    log_info(logger, "## PID: %d - FETCH - Program Counter: %d", pid, pc);
 
    enviar_int32(fd_km, OP_FETCH_INSTRUCCION);
    enviar_int32(fd_km, pid);
    enviar_int32(fd_km, (int32_t)pc);
 
    int32_t resp;
    if (!recibir_int32(fd_km, &resp) || resp != OP_OK)
    {
        log_error(logger, "FETCH: KM respondio error (PID=%d, PC=%d)", pid, pc);
        return NULL;
    }
 
    int32_t tam;
    if (!recibir_int32(fd_km, &tam) || tam <= 0)
        return NULL;
 
    char *instruccion = malloc(tam + 1);
    if (recv(fd_km, instruccion, tam, MSG_WAITALL) != tam)
    {
        free(instruccion);
        return NULL;
    }
    instruccion[tam] = '\0';
    return instruccion;
}
 
/* =========================================================
 * Operaciones de memoria sobre Memory Stick
 * =========================================================
 * La MMU traduce dirección lógica → física:
 *   num_segmento   = floor(dir_logica / SEGMENT_MAX_SIZE)
 *   desplazamiento = dir_logica % SEGMENT_MAX_SIZE
 * Para el checkpoint 1 usamos SEGMENT_MAX_SIZE=256 del config KM.
 * ========================================================= */
 
int32_t g_segment_max_size = 256; /* leído del config KM */
 
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
    int32_t dir_fisica = despl; /* dentro del MS el offset es el despl */
 
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
 
/* =========================================================
 * EXECUTE — interpreta y ejecuta una instrucción
 * Devuelve true si el ciclo debe continuar,
 *         false si el proceso debe volver al KS.
 * *motivo se setea solo cuando devuelve false.
 * ========================================================= */
 
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
 
    /* ── Instrucciones de registro ── */
    if (!strcmp(op, "NOOP")) {
        /* nada */
    }
    else if (!strcmp(op, "SET")) {
        uint32_t val = (uint32_t)atoi(a2);
        cpu_escribir_registro(&ctx->regs, a1, val);
        if (!strcmp(a1, "PC")) modifico_pc = true;
    }
    else if (!strcmp(op, "SUM")) {
        uint32_t r1 = cpu_leer_registro(&ctx->regs, a1);
        uint32_t r2 = cpu_leer_registro(&ctx->regs, a2);
        cpu_escribir_registro(&ctx->regs, a1, r1 + r2);
    }
    else if (!strcmp(op, "SUB")) {
        uint32_t r1 = cpu_leer_registro(&ctx->regs, a1);
        uint32_t r2 = cpu_leer_registro(&ctx->regs, a2);
        cpu_escribir_registro(&ctx->regs, a1, r1 - r2);
    }
    else if (!strcmp(op, "JNZ")) {
        uint32_t val = cpu_leer_registro(&ctx->regs, a1);
        if (val != 0) {
            ctx->regs.PC = (uint32_t)atoi(a2);
            modifico_pc = true;
        }
    }
    /* ── Instrucciones de memoria ── */
    else if (!strcmp(op, "MOV_IN")) {
        uint32_t buf = 0;
        cpu_leer_memoria(ctx->pid, ctx->regs.SI, sizeof(uint32_t), &buf);
        cpu_escribir_registro(&ctx->regs, a1, buf);
        log_info(logger, "PID: %d - Acción: LEER - Dirección Física: %d - Valor: %u",
                 ctx->pid, ctx->regs.SI, buf);
    }
    else if (!strcmp(op, "MOV_OUT")) {
        uint32_t val = cpu_leer_registro(&ctx->regs, a1);
        cpu_escribir_memoria(ctx->pid, ctx->regs.DI, sizeof(uint32_t), &val);
        log_info(logger, "PID: %d - Acción: ESCRIBIR - Dirección Física: %d - Valor: %u",
                 ctx->pid, ctx->regs.DI, val);
    }
    else if (!strcmp(op, "COPY_MEM")) {
        int32_t tam = (int32_t)cpu_leer_registro(&ctx->regs, a1);
        void *buf = malloc(tam);
        cpu_leer_memoria(ctx->pid, ctx->regs.SI, tam, buf);
        cpu_escribir_memoria(ctx->pid, ctx->regs.DI, tam, buf);
        free(buf);
    }
    /* ── Syscalls — devuelven proceso al KS ── */
    else if (!strcmp(op, "EXIT")) {
        *motivo = MOTIVO_EXIT;
        return false;
    }
    else if (!strcmp(op, "SLEEP") || !strcmp(op, "STDIN") ||
             !strcmp(op, "STDOUT") || !strcmp(op, "MUTEX_CREATE") ||
             !strcmp(op, "MUTEX_LOCK") || !strcmp(op, "MUTEX_UNLOCK") ||
             !strcmp(op, "MEM_ALLOC") || !strcmp(op, "MEM_FREE") ||
             !strcmp(op, "INIT_PROC")) {
        *motivo = MOTIVO_SYSCALL;
        return false;
    }
    else {
        log_warning(logger, "PID: %d - Instrucción desconocida: %s", ctx->pid, op);
    }
 
    if (!modifico_pc)
        ctx->regs.PC++;
 
    return true;
}
 
/* =========================================================
 * hilo_interrupciones
 * Escucha interrupciones asíncronas del KS sobre fd_ks.
 * Las almacena en g_irq protegido por mutex_irq.
 * ========================================================= */
 
void *hilo_interrupciones(void *arg)
{
    (void)arg;
    int32_t motivo;
    while (recibir_int32(fd_ks, &motivo))
    {
        log_info(logger, "## Interrupción recibida");
        pthread_mutex_lock(&mutex_irq);
        g_irq.activa = true;
        g_irq.motivo = motivo;
        pthread_mutex_unlock(&mutex_irq);
    }
    return NULL;
}
 
/* =========================================================
 * cpu_ciclo_instruccion
 * Loop principal: fetch → execute → check interrupt
 * Se ejecuta con un contexto ya cargado del KM.
 * ========================================================= */
 
void cpu_ciclo_instruccion(t_contexto *ctx)
{
    e_motivo_retorno motivo = MOTIVO_EXIT;
 
    while (1)
    {
        /* ── FETCH ── */
        char *instruccion = cpu_fetch(ctx->pid, ctx->regs.PC);
        if (!instruccion)
        {
            log_error(logger, "FETCH fallo (PID=%d, PC=%d)", ctx->pid, ctx->regs.PC);
            motivo = MOTIVO_SEG_FAULT;
            break;
        }
 
        /* ── EXECUTE ── */
        bool continuar = cpu_execute(ctx, instruccion, &motivo);
        free(instruccion);
 
        if (!continuar)
            break;
 
        /* ── CHECK INTERRUPT ── */
        pthread_mutex_lock(&mutex_irq);
        bool hay_irq = g_irq.activa;
        int32_t irq_motivo = g_irq.motivo;
        pthread_mutex_unlock(&mutex_irq);
 
        if (hay_irq)
        {
            log_info(logger, "## Interrupción recibida");
            motivo = (e_motivo_retorno)irq_motivo;
 
            pthread_mutex_lock(&mutex_irq);
            g_irq.activa = false;
            pthread_mutex_unlock(&mutex_irq);
            break;
        }
    }
 
    /* ── Guardar contexto en KM y devolver PID al KS ── */
    enviar_int32(fd_km, OP_SET_CONTEXTO);
    enviar_int32(fd_km, ctx->pid);
    send(fd_km, &ctx->regs, sizeof(t_registros), MSG_NOSIGNAL);
 
    int32_t ack;
    recibir_int32(fd_km, &ack);
 
    /* Devolver PID al KS con motivo */
    enviar_int32(fd_ks, ctx->pid);
    enviar_int32(fd_ks, (int32_t)motivo);
 
    log_info(logger, "## PID: %d devuelto al KS — motivo: %d", ctx->pid, motivo);
}
 
/* =========================================================
 * main
 * Uso: ./cpu cpu1.config 1
 * ========================================================= */
 
int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Uso: %s [archivo_config] [id_cpu]\n", argv[0]);
        return 1;
    }
 
    g_id_cpu = atoi(argv[2]);
 
    /* ----- Logger con identificador ---- */
    char log_name[64];
    snprintf(log_name, sizeof(log_name), "cpu%d.log", g_id_cpu);
    logger = log_create(log_name, "CPU", true, LOG_LEVEL_INFO);
    if (!logger) { fprintf(stderr, "Error al crear logger\n"); return 1; }
 
    /* ----- Config ----- */
    config = config_create(argv[1]);
    if (!config)
    {
        char alt[512];
        snprintf(alt, sizeof(alt), "../%s", argv[1]);
        config = config_create(alt);
    }
    if (!config)
    {
        log_error(logger, "No se pudo cargar '%s'", argv[1]);
        log_destroy(logger);
        return 1;
    }
 
    /* SEGMENT_MAX_SIZE del config del KM — por ahora hardcoded como 256
     * pero se puede agregar al config de la CPU si se necesita */
    if (config_has_property(config, "SEGMENT_MAX_SIZE"))
        g_segment_max_size = config_get_int_value(config, "SEGMENT_MAX_SIZE");
 
    /* ── 1. Conectar a Kernel Scheduler ── */
    char *ip_ks     = config_get_string_value(config, "IP_KERNEL_SCHEDULER");
    int   puerto_ks = config_get_int_value(config, "PUERTO_KERNEL_SCHEDULER");
 
    log_info(logger, "Conectando a Kernel Scheduler %s:%d...", ip_ks, puerto_ks);
    fd_ks = conectar_a_servidor(logger, ip_ks, puerto_ks);
    free(ip_ks);
    if (fd_ks == -1) { log_error(logger, "Fallo conexion KS"); goto cleanup; }
 
    if (!enviar_handshake(logger, fd_ks, TIPO_CPU))
    { log_error(logger, "Handshake fallo con KS"); goto cleanup; }
 
    /* Enviar ID de CPU al KS */
    enviar_int32(fd_ks, g_id_cpu);
    log_info(logger, "## CPU %d conectada al Kernel Scheduler", g_id_cpu);
 
    /* ── 2. Conectar a Kernel Memory ── */
    char *ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    int   puerto_km = config_get_int_value(config, "PUERTO_KERNEL_MEMORY");
 
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_km = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
    if (fd_km == -1) { log_error(logger, "Fallo conexion KM"); goto cleanup; }
 
    if (!enviar_handshake(logger, fd_km, TIPO_CPU))
    { log_error(logger, "Handshake fallo con KM"); goto cleanup; }
 
    enviar_int32(fd_km, g_id_cpu);
    log_info(logger, "## CPU %d conectada al Kernel Memory", g_id_cpu);
 
    /* ── 3. Conectar a Memory Stick ── */
    char *ip_ms     = config_get_string_value(config, "IP_KERNEL_MEMORY_STICK");
    int   puerto_ms = config_get_int_value(config, "PUERTO_MEMORY_STICK");
 
    log_info(logger, "Conectando a Memory Stick %s:%d...", ip_ms, puerto_ms);
    fd_ms = conectar_a_servidor(logger, ip_ms, puerto_ms);
    free(ip_ms);
    if (fd_ms == -1)
    {
        log_warning(logger, "No se pudo conectar al Memory Stick — operaciones de memoria no disponibles");
        /* No es fatal en checkpoint 1 */
    }
    else
        log_info(logger, "## CPU %d conectada al Memory Stick", g_id_cpu);
 
    /*
     * El hilo de interrupciones se implementará con un fd dedicado
     * cuando el KS envíe interrupciones por un canal separado.
     * Por ahora el check_interrupt solo verifica la variable g_irq
     * que será seteada por señales en el futuro.
     * NO lanzar el hilo aquí porque comparte fd_ks con el loop
     * principal y causaría race condition al leer el PID.
     */
 
    log_info(logger, "CPU %d lista — esperando PID del Kernel Scheduler", g_id_cpu);
 
    /* ── 5. Loop principal: esperar PID, ejecutar, devolver ── */
    int32_t pid;
    while (recibir_int32(fd_ks, &pid))
    {
        log_info(logger, "## CPU %d recibio PID %d — solicitando contexto", g_id_cpu, pid);
 
        /* Pedir contexto al KM */
        enviar_int32(fd_km, OP_GET_CONTEXTO);
        enviar_int32(fd_km, pid);
 
        int32_t resp;
        if (!recibir_int32(fd_km, &resp) || resp != OP_OK)
        {
            log_error(logger, "No se pudo obtener contexto de PID %d", pid);
            enviar_int32(fd_ks, pid);
            enviar_int32(fd_ks, (int32_t)MOTIVO_SEG_FAULT);
            continue;
        }
 
        t_contexto ctx;
        ctx.pid = pid;
        if (recv(fd_km, &ctx.regs, sizeof(t_registros), MSG_WAITALL)
            != sizeof(t_registros))
        {
            log_error(logger, "Error recibiendo contexto de PID %d", pid);
            continue;
        }
 
        log_info(logger, "## PID: %d — Contexto cargado (PC=%d)", pid, ctx.regs.PC);
 
        /* Ciclo de instrucción */
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