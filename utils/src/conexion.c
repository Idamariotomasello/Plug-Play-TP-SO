#include "conexion.h"

pthread_mutex_t mutex_io = PTHREAD_MUTEX_INITIALIZER;

sem_t g_sem_listo;

void saludar(char* quien) {
    printf("Hola desde %s!!\n", quien);
}


void inicializar_sincronizacion() {
    // Inicialización explícita (aunque los INITIALIZER ya lo hacen)
    pthread_mutex_init(&mutex_io, NULL);
}

void destruir_sincronizacion() {
    pthread_mutex_destroy(&mutex_io);
}


void inicializar_semaforos() {
    // Inicializar todos los semáforos en 0 (bloqueados)
    sem_init(&g_sem_listo, 0, 0);

}

void destruir_semaforos() {
    sem_destroy(&g_sem_listo);
}