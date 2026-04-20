#ifndef IO_H_
#define IO_H_

#include <conexion.h>

typedef enum
{
    IO_TIPO_INVALIDO = 0,
    IO_TIPO_SLEEP    = 710,
    IO_TIPO_STDIN    = 711,
    IO_TIPO_STDOUT   = 712,
} e_io_tipo;

void io_ejecutar_sleep(int32_t pid);
void io_ejecutar_stdin(int32_t pid);
void io_ejecutar_stdout(int32_t pid);
void io_loop(void);
const char *io_tipo_nombre(e_io_tipo tipo);
e_io_tipo io_tipo_desde_string(const char *s);

#endif
