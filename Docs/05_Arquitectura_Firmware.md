# SmartDose — Arquitectura de Firmware

Sistema automático de dispensación de medicamentos basado en **ESP32** con **FreeRTOS**, desarrollado con ESP-IDF y PlatformIO.

---

## Tabla de contenidos

1. [Visión general](#1-visión-general)
2. [Hardware y pines](#2-hardware-y-pines)
3. [Estructura del firmware](#3-estructura-del-firmware)
4. [Tareas FreeRTOS](#4-tareas-freertos)
5. [Módulos de software](#5-módulos-de-software)
6. [Máquina de estados por tubo](#6-máquina-de-estados-por-tubo)
7. [Flujo de dispensación](#7-flujo-de-dispensación)
8. [Protocolo de comandos](#8-protocolo-de-comandos)
9. [Persistencia — NVS](#9-persistencia--nvs)
10. [Parámetros configurables](#10-parámetros-configurables)

---

## 1. Visión general

<img width="500" height="340" alt="image" src="https://github.com/user-attachments/assets/912a8959-30b6-4489-9c71-38240b59b71f" />


SmartDose controla hasta **4 tubos dispensadores** de forma independiente. Cada tubo tiene:
- Un **servo motor** para liberar la pastilla.
- Un **sensor IR de bandeja** para confirmar la caída.
- Un **horario programado** almacenado en NVS.

La interfaz con el usuario se realiza a través de una **pantalla Nextion** (táctil) y/o una **consola UART** (Serial Monitor).

---

## 2. Hardware y pines

| Componente | GPIO | Interfaz |
|---|---|---|
| Servo Tubo 1 | 14 | LEDC / PWM |
| Servo Tubo 2 | 27 | LEDC / PWM |
| Servo Tubo 3 | 16 | LEDC / PWM |
| Servo Tubo 4 | 17 | LEDC / PWM |
| IR Bandeja Tubo 1 | 36 | GPIO entrada |
| IR Bandeja Tubo 2 | 39 | GPIO entrada |
| IR Bandeja Tubo 3 | 25 | GPIO entrada |
| IR Bandeja Tubo 4 | 26 | GPIO entrada |
| Buzzer | 13 | GPIO salida |
| LED de alerta | 12 | GPIO salida |
| I2C SDA (DS3231) | 21 | I2C Master |
| I2C SCL (DS3231) | 22 | I2C Master |
| UART0 TX/RX | — | Consola de configuración (115200 bps) |
| UART1 TX (→ Nextion) | 5 | UART1 (9600 bps) |
| UART1 RX (← Nextion) | 4 | UART1 (9600 bps) |

### Periféricos y drivers ESP-IDF

| Periférico | Driver | Uso |
|---|---|---|
| Servos x4 | `driver/ledc` | PWM 50 Hz, resolución 14 bits |
| Sensores IR x4 | `driver/gpio` | Entrada digital con pull-up |
| Buzzer + LED | `driver/gpio` | Salida digital |
| DS3231 RTC | `driver/i2c` | Lectura/escritura hora actual |
| Consola USB | `driver/uart` (UART0) | Comandos de configuración |
| Nextion HMI | `driver/uart` (UART1) | Interfaz táctil |
| Persistencia | `nvs_flash` / `nvs` | Horarios y flag RTC |

---

## 3. Estructura del firmware

Todo el firmware reside en **`main.c`**, organizado en secciones funcionales:

```
main.c
│
├── Definiciones de pines y parámetros
├── Estructuras de datos (horario_t, info_tubo_t, estado_tubo_t)
├── Variables globales compartidas + mutex FreeRTOS
│
├── [Módulo] Servo          — angulo_a_duty(), servo_set(), servo_dispensar()
├── [Módulo] Sensor IR      — sensor_leer() con debounce, bandeja_tiene_pastilla()
├── [Módulo] Alertas        — buzzer_set(), led_set(), actualizar_salidas_alarma()
├── [Módulo] RTC DS3231     — ds3231_leer(), ds3231_get_hora(), ds3231_set_hora()
├── [Módulo] Nextion HMI    — nextion_cmd(), nextion_set_txt(), nextion_actualizar_pantalla()
├── [Módulo] NVS            — nvs_guardar(), nvs_cargar(), rtc_ya_configurado()
├── [Módulo] Log            — log_evento() → printf + Nextion
├── [Módulo] Dispensación   — ciclo_dispensacion(), solicitar_dispensacion()
├── [Módulo] Comandos       — procesar_comando(), configurar_horario(), ack_tubo()
│
└── Tareas FreeRTOS
    ├── tarea_scheduler
    ├── tarea_monitor
    ├── tarea_uart_monitor
    ├── tarea_nextion_rx
    ├── tarea_nextion_update
    └── tarea_dispensacion (creada dinámicamente)
```

### Estructuras de datos principales

```c
// Estado operacional de un tubo
typedef enum { TUBO_OK, TUBO_DISPENSANDO, TUBO_FALLO } estado_tubo_t;

// Horario programado por el usuario
typedef struct {
    bool activo;
    int  hora;
    int  minuto;
    int  cantidad;   // 1–10 pastillas
} horario_t;

// Estado en tiempo real de un tubo
typedef struct {
    estado_tubo_t estado;
    bool          pastilla_en_bandeja;
    bool          alerta_activa;
} info_tubo_t;
```

**Protección de concurrencia:** todas las lecturas/escrituras sobre `g_horarios[]` y `g_info[]` están protegidas con un **mutex FreeRTOS** (`g_mutex`).

---

## 4. Tareas FreeRTOS

| Tarea | Prioridad | Stack | Período | Función |
|---|---|---|---|---|
| `tarea_scheduler` | 6 | 4 KB | 1 s | Compara hora RTC con horarios; lanza dispensación automática |
| `tarea_nextion_rx` | 5 | 4 KB | Reactiva | Lee comandos enviados desde la Nextion vía UART1 |
| `tarea_monitor` | 4 | 4 KB | 7 s | Verifica estado de bandejas; actualiza alertas visuales/sonoras |
| `tarea_uart_monitor` | 3 | 4 KB | Reactiva | Lee comandos desde consola UART0 (Serial Monitor) |
| `tarea_nextion_update` | 2 | 4 KB | 1.5 s | Refresca la pantalla Nextion con hora, estados y horarios |
| `tarea_dispensacion` | 7 | 4 KB | Bajo demanda | Creada dinámicamente por tubo; se elimina al terminar |

> **Nota:** `tarea_dispensacion` se crea con `xTaskCreate()` cuando se solicita una dispensación (por horario o manual). Se auto-elimina con `vTaskDelete(NULL)` al completar el ciclo.

---

## 5. Módulos de software

### 5.1 Servo (LEDC PWM)

- Configuración: **50 Hz**, resolución **14 bits**, rango de pulso **600–2500 µs**.
- La función `servo_dispensar(idx, cantidad)` alterna entre ángulo abierto (90°) y cerrado (0°), con `SERVO_MS_ACCION = 400 ms` entre posiciones.
- Fórmula de conversión:

```c
us = SERVO_MIN_US + (SERVO_MAX_US - SERVO_MIN_US) * angulo / 180
duty = (us * 16383) / 20000
```

### 5.2 Sensor IR de bandeja (FC-51)

- Lectura con **anti-rebote por software**: toma `DEBOUNCE_COUNT = 3` muestras con 10 ms entre cada una; retorna `true` si la mayoría detecta objeto.
- Lógica configurable: `SENSOR_ACTIVO_EN_LOW = 1` (LOW = objeto presente, comportamiento estándar FC-51).

### 5.3 RTC DS3231

- Comunicación **I2C** a 400 kHz, dirección `0x68`.
- Funciones: `ds3231_get_hora()`, `ds3231_set_hora()`, `ds3231_leer()`.
- Conversión **BCD ↔ decimal** implementada manualmente.
- Al arrancar, si el RTC no fue configurado previamente (flag en NVS), se inicializa a una hora por defecto.

### 5.4 Nextion HMI

- Comunicación **UART1** a 9600 bps.
- Protocolo Nextion: comandos ASCII terminados con `0xFF 0xFF 0xFF`.
- Funciones de escritura: `nextion_set_txt(objeto, texto)`, `nextion_set_val(objeto, valor)`, `nextion_cmd(formato, ...)`.
- La función `nextion_actualizar_pantalla()` refresca: hora RTC, estado de cada tubo, contenido de bandeja, horario activo y mensaje global.
- Páginas usadas:
  - `page0` → pantalla principal
  - `page2` → alerta de dosis lista
  - `page3` → pantalla de error

### 5.5 Alertas (Buzzer + LED)

- `actualizar_salidas_alarma()` enciende LED y buzzer si **cualquier** tubo tiene `alerta_activa = true` o `pastilla_en_bandeja = true`.
- En error de dispensación: alarma activa por **15 segundos** (`ERROR_ALARMA_MS`), luego se apaga automáticamente aunque el estado `TUBO_FALLO` persiste hasta ACK manual.

### 5.6 Persistencia (NVS)

- Espacio NVS: namespace `"smartdose"`.
- Claves almacenadas:
  - `"horarios"` → string serializado con formato `T1=08:30:2;T3=20:00:1`
  - `"rtc_set"` → flag `uint8_t` (1 = RTC ya fue configurado)
- Se carga automáticamente al arrancar (`nvs_cargar()`).
- Se guarda cada vez que se configura o borra un horario (`nvs_guardar()`).

---

## 6. Máquina de estados por tubo

<img width="512" height="381" alt="image" src="https://github.com/user-attachments/assets/23d4d126-90b2-48dd-9b3f-cc716969c821" />


---

## 7. Flujo de dispensación

```
solicitar_dispensacion(idx)
        │
        ├── ¿tubo ya en TUBO_DISPENSANDO? → Rechazar
        │
        └── xTaskCreate(tarea_dispensacion)
                    │
                    ▼
            ciclo_dispensacion(idx)
                    │
                    ├── [1] Estado → TUBO_DISPENSANDO
                    │
                    ├── [2] Bucle reintentos (hasta MAX_REINTENTOS_CAIDA = 4)
                    │        │
                    │        ├── servo_dispensar(idx, cantidad)
                    │        │       └── Open → delay 400ms → Close → delay 400ms
                    │        │
                    │        └── Esperar detección en bandeja
                    │               ├── ¿sensor activo antes de TIMEOUT_CAIDA_MS (3 s)? → Éxito ✓
                    │               └── Timeout → siguiente reintento
                    │
                    ├── [3a] ÉXITO: Estado → TUBO_OK, alerta_activa = true
                    │         Esperar retiro de pastilla
                    │         ├── bandeja vacía → apagar alarma, ciclo completado
                    │         └── Timeout TIMEOUT_RECOGIDA_MS (60 s) → log WARN, reiniciar timer
                    │
                    └── [3b] FALLO: Estado → TUBO_FALLO, alerta_activa = true
                              Alarma 15 s → apagar alarma, mantener TUBO_FALLO
```

---

## 8. Protocolo de comandos

Los comandos son aceptados desde **UART0** (consola) y desde **Nextion** (prefijo `nxt:` ignorado automáticamente). No distinguen mayúsculas/minúsculas.

| Comando | Descripción | Ejemplo |
|---|---|---|
| `CFG,tubo,hora,min,cantidad` | Configura horario de un tubo | `CFG,1,8,30,1` |
| `T<n>=HH:MM:C` | Formato alternativo de horario | `T2=20:00:2` |
| `DISP<n>` | Dispensación manual inmediata | `DISP3` |
| `ACK<n>` | Confirma y limpia alerta de un tubo | `ACK1` |
| `SETTIME=HH:MM:SS` | Sincroniza el RTC DS3231 | `SETTIME=09:15:00` |
| `STATUS` / `REFRESH` | Imprime estado completo por consola + refresca Nextion | `STATUS` |
| `CLEAR` / `BORRAR` | Borra todos los horarios (NVS incluido) | `CLEAR` |

**Validaciones aplicadas:**
- Tubo: 1–4
- Hora: 0–23, Minuto: 0–59
- Cantidad: 1–10 pastillas

---

## 9. Persistencia — NVS

```
NVS namespace: "smartdose"
│
├── "horarios"  →  "T1=08:30:1;T3=20:00:2"   (string, máx 256 bytes)
└── "rtc_set"   →  0x01                        (uint8, flag de configuración)
```

- Los horarios se serializan en texto plano separados por `;`.
- Al arrancar, `nvs_cargar()` parsea el string y reconstruye `g_horarios[]`.
- Si no hay horarios guardados, el valor almacenado es la cadena `"VACIO"`.

---

## 10. Parámetros configurables

Todos los parámetros clave están definidos como `#define` al inicio del archivo:

| Parámetro | Valor | Descripción |
|---|---|---|
| `MAX_TUBOS` | 4 | Número de tubos dispensadores |
| `SERVO_FREQ_HZ` | 50 | Frecuencia PWM del servo |
| `SERVO_MIN_US` | 600 | Pulso mínimo servo (µs) |
| `SERVO_MAX_US` | 2500 | Pulso máximo servo (µs) |
| `SERVO_ANGULO_ABIERTO` | 90° | Ángulo de apertura del tubo |
| `SERVO_ANGULO_CERRADO` | 0° | Ángulo de cierre del tubo |
| `SERVO_MS_ACCION` | 400 ms | Tiempo entre posiciones del servo |
| `TIMEOUT_CAIDA_MS` | 3000 ms | Tiempo máximo para detectar caída de pastilla |
| `TIMEOUT_RECOGIDA_MS` | 60000 ms | Tiempo máximo de espera antes de re-alertar |
| `ERROR_ALARMA_MS` | 15000 ms | Duración de alarma en caso de fallo |
| `MAX_REINTENTOS_CAIDA` | 4 | Intentos de dispensación antes de declarar fallo |
| `DEBOUNCE_COUNT` | 3 | Muestras para anti-rebote del sensor IR |
| `TICK_SCHEDULER_MS` | 1000 ms | Período de revisión del scheduler |
| `TICK_MONITOR_MS` | 7000 ms | Período de monitoreo de bandejas |
| `TICK_NEXTION_MS` | 1500 ms | Período de refresco de pantalla |
| `SENSOR_ACTIVO_EN_LOW` | 1 | Lógica del sensor IR (1 = LOW activo) |
