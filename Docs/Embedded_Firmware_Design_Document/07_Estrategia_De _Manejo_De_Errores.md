# Estrategia de Manejo de Errores

Descripción completa de los errores detectados por el firmware, su origen, y las acciones correctivas aplicadas automáticamente.

---

## Tabla de contenidos

1. [Principios generales](#1-principios-generales)
2. [Catálogo de errores](#2-catálogo-de-errores)
   - [ERROR 01 — Fallo de dispensación](#error-01--fallo-de-dispensación)
3. [Resumen consolidado](#3-resumen-consolidado)
4. [Códigos de log](#4-códigos-de-log)

---

## 1. Principios generales

- **Recuperación automática primero:** el firmware intenta resolver el error sin intervención humana antes de escalar la alerta.
- **Sin bloqueo del sistema:** un error en un tubo no afecta la operación de los demás.
- **Trazabilidad:** todo evento relevante se registra en el buffer circular de logs en memoria (`g_logs[5][120]`) y se refleja en la pantalla Nextion en tiempo real mediante `nextion_actualizar_logs()`.
- **Alarma proporcional:** el LED y el buzzer permanecen activos solo el tiempo estrictamente necesario según el tipo de error.
- **Estado persistente:** el estado `TUBO_FALLO` se mantiene visible en pantalla y consola hasta que el operador lo confirme con `ACK<n>` o expire el temporizador automático de 15 segundos.

---

## 2. Catálogo de errores

---

### ERROR 01 — Fallo de dispensación

**Descripción:** el sensor IR de bandeja no detecta la pastilla tras ejecutar el ciclo completo de apertura (180°) y cierre (0°) del servo, agotando todos los reintentos disponibles.

**Posibles causas físicas:** atasco mecánico, tubo vacío, falla del motor.

**Origen en código:** `dispensar_con_reintentos_y_confirmar_bandeja()` → `ciclo_dispensacion()` → `log_error_01()`

**Mensaje de log:**
```
HH:MM:SS | ERROR 01 | T<n> | Falla de dispensacion: no se detecto dosis; posible atasco, tubo vacio o falla del motor
```

**Secuencia de manejo:**

```
servo_dispensar(idx, cantidad)
        │
        └── Esperar detección en bandeja (TIMEOUT_CAIDA_MS = 3 s)
                │
                ├── Detectado → Éxito, continúa flujo normal
                │
                └── Timeout → WARN en log → siguiente intento
                        │
                        └── (hasta MAX_REINTENTOS_CAIDA = 4 intentos)
                                │
                                └── Todos fallidos
                                        │
                                        ├── log_error_01()
                                        │       "ERROR 01 | Falla de dispensacion..."
                                        ├── Estado → TUBO_FALLO
                                        ├── alerta_activa = true
                                        ├── Nextion → page3 (pantalla de error)
                                        │       tErrorMsg = "ERROR 01: Tubo X - falla de dispensacion..."
                                        ├── LED + Buzzer ON
                                        ├── Esperar ERROR_ALARMA_MS (15 s)
                                        └── LED + Buzzer OFF
                                            alerta_activa = false
                                            (TUBO_FALLO persiste hasta ACK manual)
```

**Acción del operador requerida:** comando `ACK<n>` por consola o botón en Nextion para restablecer el tubo a `TUBO_OK`.

---

## 3. Resumen consolidado

| # | Error | Detección | Reintentos automáticos | Alarma | Duración alarma | Requiere ACK manual |
|---|---|---|---|---|---|---|
| 01 | Fallo de dispensación | Sensor IR bandeja | 4 intentos (3 s c/u) | LED + Buzzer + page3 | 15 s | Sí |

---

## 4. Códigos de log

El sistema registra eventos con el siguiente formato:

```
HH:MM:SS | NIVEL [CODIGO] | T<n> / SYS | Mensaje
```

| Código | Nivel | Descripción |
|---|---|---|
| `01` | `ERROR` | Falla de dispensación: no se detectó dosis tras 4 intentos; posible atasco, tubo vacío o falla del motor |
| — | `WARN` | No se detectó pastilla en intento individual (antes de agotar reintentos) |
| — | `WARN` | Dosis en bandeja, esperando retiro (al iniciar espera) |
| — | `WARN` | Pastilla no recogida (se repite cada 60 s) |
| — | `INFO` | Inicio de dispensación |
| — | `INFO` | Tubo X intento N/4 |
| — | `INFO` | Pastilla retirada — ciclo completado |
| — | `INFO` | Horario guardado |
| — | `INFO` | Horarios borrados — sistema sin dispensaciones |

Los últimos **5 eventos** se mantienen en el buffer circular `g_logs[5][120]` y se muestran en la Nextion en los campos `tLog1` a `tLog5`, actualizándose en cada llamada a `nextion_actualizar_logs()`.
