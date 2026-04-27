#ifndef IO_H_
#define IO_H_

#include <conexion.h>


void io_ejecutar_sleep(int32_t pid);
void io_ejecutar_stdin(int32_t pid);
void io_ejecutar_stdout(int32_t pid);
void io_loop(void);
const char *io_tipo_nombre(op_code tipo);
op_code io_tipo_desde_string(const char *s);

#endif
