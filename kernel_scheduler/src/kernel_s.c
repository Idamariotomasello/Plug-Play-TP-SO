#include "kernel_s.h"
#include <cliente.h>
#include <server.h>
/*
int main(int argc, char* argv[]) {
    saludar("kernel_scheduler");
    return 0;
}
*/

// Hilo para atender CPUs
void *atender_cpu(void *varg) {
    t_hilo_arg *arg = (t_hilo_arg *)varg;
    int fd_cpu = arg->fd_cliente;
    free(arg);
    
    log_info(logger, "## CPU conectada (fd=%d)", fd_cpu);
    
    int32_t id_cpu;
    if (!recibir_int32(fd_cpu, &id_cpu)) {
        log_error(logger, "Error al recibir ID de CPU (fd=%d)", fd_cpu);
        close(fd_cpu);
        return NULL;
    }
    
    log_info(logger, "CPU %d Conectada (fd=%d)", id_cpu, fd_cpu);
    
    int32_t cod_op;
    while (recibir_int32(fd_cpu, &cod_op)) {
        log_info(logger, "CPU %d — cod_op recibido: %d", id_cpu, cod_op);
        // TODO: manejar operaciones futuras
    }
    
    log_info(logger, "CPU %d desconectada (fd=%d)", id_cpu, fd_cpu);
    close(fd_cpu);
    return NULL;
}

// Servidor multihilo para CPUs/IOs
void iniciar_servidor_puerto_escucha() {
    char *puerto_str = config_get_string_value(config, "PUERTO_ESCUCHA");
    int puerto = atoi(puerto_str);
    free(puerto_str);
    
    int fd_escucha = iniciar_servidor(logger, puerto);
    if (fd_escucha == -1) {
        log_error(logger, "No se pudo iniciar servidor Kernel Scheduler puerto %d", puerto);
        return;
    }
    
    log_info(logger, "Kernel Scheduler esperando CPUs/IOs en puerto %d (fd=%d)", puerto, fd_escucha);
    
    while (1) {
        int fd_cliente = esperar_cliente(logger, fd_escucha);
        if (fd_cliente == -1) continue;
        
        t_hilo_arg *arg = malloc(sizeof(t_hilo_arg));
        if (!arg) {
            log_error(logger, "Sin memoria hilo cliente (fd=%d)", fd_cliente);
            close(fd_cliente);
            continue;
        }
        
        arg->logger = logger;
        arg->fd_cliente = fd_cliente;
        
        pthread_t hilo;
        pthread_create(&hilo, NULL, atender_cpu, arg);
        pthread_detach(hilo);
    }
    
    close(fd_escucha);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s kernel_s.config [prueba]\n", argv[0]);
        fprintf(stderr, "Ej: %s kernel_s.config prueba\n", argv[0]);
        return 1;
    }
    
    char *nombre_config = argv[1];
    
    // Logger
    logger = log_create("kernel_scheduler.log", "KERNEL_SCHEDULER", true, LOG_LEVEL_INFO);
    if (!logger) {
        fprintf(stderr, "Error creando logger\n");
        return 1;
    }
    
    // Cargar kernel_s.config (múltiples rutas)
    config = config_create(nombre_config);
    if (!config) {
        char ruta_alt[512];
        snprintf(ruta_alt, sizeof(ruta_alt), "../%s", nombre_config);
        config = config_create(ruta_alt);
        if (!config) {
            log_error(logger, "No se pudo cargar kernel_s.config (ni %s)", ruta_alt);
            log_destroy(logger);
            return 1;
        }
    }
    
    log_info(logger, "Kernel Scheduler iniciando...");
    
    // Conectar Kernel Memory
    char *ip_km = config_get_string_value(config, "IP_KERNEL_MEMORY");
    char *puerto_km_str = config_get_string_value(config, "PUERTO_KERNEL_MEMORY");
    int puerto_km = atoi(puerto_km_str);
    free(puerto_km_str);
    
    log_info(logger, "Conectando a Kernel Memory %s:%d...", ip_km, puerto_km);
    fd_kernel_memory = conectar_a_servidor(logger, ip_km, puerto_km);
    free(ip_km);
    
    if (fd_kernel_memory == -1) {
        log_error(logger, "Fallo conexión Kernel Memory %s:%d", ip_km, puerto_km);
        config_destroy(config);
        log_destroy(logger);
        return 1;
    }
    
    // Handshake TIPO_KS
    if (!enviar_handshake(logger, fd_kernel_memory, TIPO_KS)) {
        log_error(logger, "Handshake falló con Kernel Memory");
        close(fd_kernel_memory);
        config_destroy(config);
        log_destroy(logger);
        return 1;
    }
    
    log_info(logger, " Conectado a Kernel Memory (fd=%d)", fd_kernel_memory);
    
    // Script "prueba" → KM arma path completo
    char *script_nombre = (argc > 2) ? argv[2] : "prueba";
    log_info(logger, "Creando PID 0 con script: %s", script_nombre);
    log_info(logger, "[KM] armará: SCRIPTS_BASEPATH/%s", script_nombre);
    
    // TODO: paquete CREATE_PROCESS (PID=0, script_nombre)
    
    printf("SISTEMA LISTO - Esperando conexiones CPUs (puerto %d)...\n", 
           config_get_int_value(config, "PUERTO_ESCUCHA"));
    log_info(logger, "A la espera de conexiones CPUs/IOs");
    
    // Servidor permanente
    pthread_t hilo_servidor;
    if (pthread_create(&hilo_servidor, NULL, (void*)iniciar_servidor_puerto_escucha, NULL) != 0) {
        log_error(logger, "Error creando hilo servidor");
        close(fd_kernel_memory);
        config_destroy(config);
        log_destroy(logger);
        return 1;
    }
    
    pthread_join(hilo_servidor, NULL);
    
    log_info(logger, "Kernel Scheduler finalizando...");
    close(fd_kernel_memory);
    config_destroy(config);
    log_destroy(logger);
    return 0;
}