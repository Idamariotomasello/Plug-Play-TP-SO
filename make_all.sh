#!/bin/bash

# Orden de compilación: utils primero (dependencia de todos los demás)
carpetas=("utils" "io" "swap" "memory_stick" "kernel_memory" "kernel_scheduler" "cpu")

# Colores
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

compilar_modulo() {
    local carpeta=$1
    echo -e "${YELLOW}Compilando $carpeta...${NC}"

    cd "$carpeta" || { echo -e "${RED}❌ Error al entrar en $carpeta${NC}"; exit 1; }

    if [ -d "obj/" ]; then
        make clean || { echo -e "${RED}❌ Error al limpiar $carpeta${NC}"; exit 1; }
    fi

    if make debug 2>/dev/null; then
        echo -e "${GREEN}✅ $carpeta compilado en modo debug.${NC}"
    else
        echo -e "${YELLOW}⚠️  No se encontró 'debug', intentando 'make all'...${NC}"
        if make all; then
            echo -e "${GREEN}✅ $carpeta compilado con make all.${NC}"
        else
            echo -e "${RED}❌ Error al compilar $carpeta.${NC}"
            exit 1
        fi
    fi

    cd ..
    echo "-----------------------------------"
}

# Verificar que el script se ejecuta desde la raíz del repo
for carpeta in "${carpetas[@]}"; do
    if [ ! -d "$carpeta" ]; then
        echo -e "${RED}❌ Directorio '$carpeta' no encontrado.${NC}"
        echo "   Ejecutá este script desde la raíz del repositorio."
        exit 1
    fi
done

# Compilar utils primero como biblioteca estática
compilar_modulo "utils"

# Compilar el resto en orden
for carpeta in "${carpetas[@]:1}"; do
    compilar_modulo "$carpeta"
done

echo -e "${GREEN}✅ Todas las compilaciones finalizaron correctamente.${NC}"