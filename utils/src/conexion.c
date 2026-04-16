#include "conexion.h"


sem_t g_sem_listo;

void saludar(char* quien) {
    printf("Hola desde %s!!\n", quien);
}


void inicializar_semaforos() {
    // Inicializar todos los semáforos en 0 (bloqueados)
    sem_init(&g_sem_listo, 0, 0);
}

void destruir_semaforos() {
    sem_destroy(&g_sem_listo);
}