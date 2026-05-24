<img width="804" height="662" alt="image" src="https://github.com/user-attachments/assets/3af115f0-d9db-4d3d-8e17-b17717311782" />

## Diagrama de Estados

Esta máquina de estados permite organizar la lógica del sistema de forma estructurada, garantizando control sobre cada etapa del proceso y manejo adecuado de errores.

El modelo está compuesto por cuatro estados principales: IDLE, DISPENSING, WAITING_PICKUP y ERROR.

### IDLE

El estado IDLE representa el estado de reposo del sistema. En este estado, el dispositivo realiza monitoreo continuo de la hora mediante el módulo RTC DS3231 y permanece a la espera de una condición de activación.

La transición desde IDLE ocurre cuando se alcanza una hora programada de dispensación.

### DISPENSING

En el estado DISPENSING, el sistema activa el servomotor correspondiente para liberar la cantidad de pastillas configuradas. Durante este estado se implementa la lógica de reintentos y verificación de caída de la pastilla utilizando el sensor infrarrojo de bandeja.

Las posibles transiciones desde este estado son:
- Hacia WAITING_PICKUP, si la pastilla es detectada correctamente en la bandeja.
- Hacia ERROR, si se agotan los 4 intentos del servomotor para dispensar y no hay detectacción de la caída de la pastilla.

### WAITING_PICKUP

El estado WAITING_PICKUP se activa cuando la pastilla ha sido dispensada exitosamente y se encuentra en la bandeja. En este estado, el sistema activa señales de alerta (LED y buzzer) para notificar al usuario que la dosis está lista.

El sistema permanece en este estado hasta que el sensor infrarrojo detecta que la bandeja está vacía, lo cual indica que el usuario ha retirado la pastilla. 

Una vez confirmada la retirada de la pastilla, el sistema desactiva las alertas y retorna al estado IDLE.

### ERROR

El estado ERROR se activa cuando el sistema no logra dispensar correctamente la pastilla después de 4 intentos.

En este estado:
- Se activan alertas visuales y auditivas durante 15 segundos.
- Se muestra un mensaje de error en la interfaz Nextion.
- Se registra el evento en el sistema de logging.

El sistema permanece en este estado durante un tiempo determinado, las alertas se activan durante 15 segundos y el usuario debe confirmar la lectura de la alerta en la interfaz gráfica. Posteriormente, el sistema desactiva las alertas y retorna al estado IDLE, permitiendo continuar la operación.

### Consideraciones de Diseño

- La máquina de estados es replicable para cada uno de los tubos del sistema, permitiendo operación independiente por canal.
- La verificación de dispensación se implementa como lógica interna dentro del estado DISPENSING, reduciendo la complejidad del modelo.
- El diseño garantiza que el sistema nunca quede bloqueado permanentemente en un estado de error, permitiendo recuperación automática o manual.
- Se asegura trazabilidad mediante registro de eventos en cada transición relevante.
