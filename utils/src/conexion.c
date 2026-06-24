#include "conexion.h"

pthread_mutex_t mutex_io = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_procesos = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_irq = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_km = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_mutex_cola_ready = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_ms_lista = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_fd_ks = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_interrupcion_pendiente = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_km_socket = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_huecos    = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_fd_swap   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_ms_ops[MAX_MS] = { PTHREAD_MUTEX_INITIALIZER };

sem_t g_sem_listo;
sem_t sem_km_cpu;
sem_t sem_cpu_ks;
sem_t sem_ks_km;
sem_t sem_km_ks;

t_nodo_ready    *g_cola_ready_head = NULL;
t_nodo_ready    *g_cola_ready_tail = NULL;

void saludar(char* quien) {
    printf("Hola desde %s!!\n", quien);
}


void inicializar_sincronizacion() {
    // Inicialización explícita (aunque los INITIALIZER ya lo hacen)
    pthread_mutex_init(&mutex_io, NULL);
    pthread_mutex_init(&mutex_procesos, NULL);
    pthread_mutex_init(&mutex_irq, NULL);
    pthread_mutex_init(&mutex_km, NULL);
    pthread_mutex_init(&mutex_ms_lista, NULL);
    pthread_mutex_init(&mutex_fd_ks, NULL);
    pthread_mutex_init(&mutex_interrupcion_pendiente, NULL);
    pthread_mutex_init(&mutex_km_socket, NULL);
    pthread_mutex_init(&mutex_huecos, NULL);
    pthread_mutex_init(&mutex_fd_swap, NULL);
    for (int i = 0; i < MAX_MS; i++)    
        pthread_mutex_init(&mutex_ms_ops[i], NULL);
}

void destruir_sincronizacion() {
    pthread_mutex_destroy(&mutex_io);
    pthread_mutex_destroy(&mutex_procesos);
    pthread_mutex_destroy(&mutex_irq);
    pthread_mutex_destroy(&mutex_km);
    pthread_mutex_destroy(&mutex_ms_lista);
    pthread_mutex_destroy(&mutex_fd_ks);
    pthread_mutex_destroy(&mutex_interrupcion_pendiente);
    pthread_mutex_destroy(&mutex_km_socket);
    pthread_mutex_destroy(&mutex_huecos);
    pthread_mutex_destroy(&mutex_fd_swap);
}


void inicializar_semaforos() {
    sem_init(&g_sem_listo, 0, 0);
    sem_init(&sem_km_cpu, 0, 0);
    sem_init(&sem_cpu_ks, 0, 0);
    sem_init(&sem_ks_km,  0, 0);
    sem_init(&sem_km_ks,  0, 0);

}

void destruir_semaforos() {
    sem_destroy(&g_sem_listo);
    sem_destroy(&sem_km_cpu);
    sem_destroy(&sem_cpu_ks);
    sem_destroy(&sem_ks_km);
    sem_destroy(&sem_km_ks);
}