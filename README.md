# Plug & Pray 

### Sistema Operativo Distribuido — Sistemas Operativos | 2026

Trabajo práctico cuatrimestral desarrollado en equipo para la materia **Sistemas Operativos**, cuyo objetivo fue implementar una simulación distribuida de un sistema operativo utilizando diferentes módulos independientes desarrollados en **C** y comunicados mediante sockets.

## 🧩 Mi participación — Módulo CPU

Mi principal responsabilidad dentro del proyecto fue el desarrollo del **módulo CPU**, encargado de simular el funcionamiento de un procesador y ejecutar las instrucciones de los procesos.

La CPU implementa un ciclo de instrucción simplificado:

```text
FETCH → DECODE → EXECUTE → CHECK INTERRUPT
```

### Principales funcionalidades desarrolladas

* Implementación del **ciclo de instrucción**.
* Manejo del **Program Counter (PC)** y registros de CPU.
* Fetch de instrucciones desde **Kernel Memory**.
* Decodificación e interpretación de instrucciones.
* Ejecución de operaciones aritméticas y de control:

  * `SET`
  * `SUM`
  * `SUB`
  * `JNZ`
  * `NOOP`
* Implementación de operaciones de acceso a memoria:

  * `MOV_IN`
  * `MOV_OUT`
  * `COPY_MEM`
* Implementación y comunicación de **Syscalls**:

  * `MUTEX_CREATE`
  * `MUTEX_LOCK`
  * `MUTEX_UNLOCK`
  * `MEM_ALLOC`
  * `MEM_FREE`
  * `SLEEP`
  * `STDIN`
  * `STDOUT`
  * `INIT_PROC`
  * `EXIT`
* Manejo de **interrupciones** provenientes del Kernel Scheduler.
* Actualización y recuperación del contexto de ejecución.
* Implementación de una **MMU simulada** para traducir direcciones lógicas a físicas utilizando segmentación.
* Validación de límites de segmentos y detección de **Segmentation Fault**.
* Comunicación con **Kernel Scheduler, Kernel Memory y Memory Stick** mediante sockets.

La CPU debía mantener registros como `PC`, `AX`, `BX`, `CX`, `DX`, `EAX`, `EBX`, `ECX`, `EDX`, `SI` y `DI`, y utilizar `SI` y `DI` para operaciones relacionadas con direcciones lógicas de memoria.

La traducción de memoria se realizó bajo un esquema de **segmentación**, separando número de segmento y desplazamiento para obtener la dirección física correspondiente.

## 🛠️ Tecnologías y conceptos

* **C**
* Linux
* POSIX
* Sockets TCP
* Threads
* Concurrencia
* Inter-Process Communication
* Gestión de memoria
* Segmentación
* MMU
* CPU / Instruction Cycle
* Git
* Make / Makefiles
* Logging

## 🎯 Qué aprendí

El desarrollo del módulo CPU me permitió llevar a código conceptos de **Arquitectura de Computadores y Sistemas Operativos**, especialmente:

* Ciclo de instrucción.
* Registros y Program Counter.
* Interrupciones.
* Syscalls.
* Comunicación entre procesos.
* Traducción de direcciones lógicas a físicas.
* Segmentación de memoria.
* Programación concurrente.
* Integración de componentes dentro de un sistema distribuido.

## 👥 Proyecto grupal

El proyecto fue desarrollado en equipo e integrado junto con los módulos:

**Kernel Scheduler · Kernel Memory · Memory Stick · SWAP · I/O · CPU**

El trabajo fue desarrollado de manera iterativa e incremental hasta lograr la integración de todos los componentes.

> **Nota:** Este repositorio corresponde a una copia personal del trabajo práctico grupal, conservada con fines de portfolio y referencia profesional, con autorización de la cátedra.
