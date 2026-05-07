#include "conexion.h"

pthread_mutex_t mutex_io = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_procesos = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_irq = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_km = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_mutex_cola_ready = PTHREAD_MUTEX_INITIALIZER;

sem_t g_sem_listo;

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
}

void destruir_sincronizacion() {
    pthread_mutex_destroy(&mutex_io);
    pthread_mutex_destroy(&mutex_procesos);
    pthread_mutex_destroy(&mutex_irq);
    pthread_mutex_destroy(&mutex_km);
}


void inicializar_semaforos() {
    // Inicializar todos los semáforos en 0 (bloqueados)
    sem_init(&g_sem_listo, 0, 0);

}

void destruir_semaforos() {
    sem_destroy(&g_sem_listo);
}