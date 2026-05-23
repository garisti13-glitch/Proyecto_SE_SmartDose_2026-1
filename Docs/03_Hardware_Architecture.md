# Hardware Architecture
El sistema SmartDose fue diseñado alrededor de un ESP32 como unidad de procesamiento, integrando sensores IR, actuadores servo, interfaz HMI y módulo RTC para dispensación automática de medicamentos.

<img width="200" height="300" alt="image" src="https://github.com/user-attachments/assets/2a2610a6-54dc-4407-beb7-b2ad52927341" />

### Tabla de componentes

| COMPONENTE     | FUNCIÓN |
|---------------|--------|
| ESP32         | Unidad de control principal del sistema |
| DS3231        | Módulo de reloj en tiempo real (RTC) |
| Sensor IR     | Detección de salida de medicamento |
| Servo SG90    | Actuador para dispensación de pastillas |
| Nextion (NX4832K035) | Interfaz HMI para interacción con el usuario |
| Buzzer    | Generación de alertas auditivas |
| LEDs    | Indicación visual de estados del sistema |
| Fuentes de alimentación | Suministro y regulación de energía del sistema |

### Módulos
#### -ESP32
Fue seleccionado debido a su capacidad multitarea mediante FreeRTOS, disponibilidad de múltiples interfaces UART e I2C y suficiente capacidad de procesamiento para el control.

<img width="200" height="200" alt="image" src="https://github.com/user-attachments/assets/b3898cc1-b08f-40e6-ac8a-d39de1bb4d86" />

#### -DS3231
Proporciona una referencia temporal precisa para la programación de dispensación.

<img width="200" height="200" alt="WhatsApp Image 2026-05-23 at 4 03 49 PM" src="https://github.com/user-attachments/assets/ed00c211-3b52-4262-a956-096fb5ee389f" />

#### -Sensores IR
Cumple la función de detectar si la pastilla salió o no durante el proceso de dispensación. Es un tipo de sensor infrarrojo reflectivo y se conecta por medio de un GPIO del ESP32
Cada tubo cuenta con un sensor que permite verificar la correcta liberación del medicamento, siendo un componente clave para la detección de errores.

<img width="200" height="200" alt="image" src="https://github.com/user-attachments/assets/9f77fdd6-d695-466b-b394-2078f5113e9f" />

#### -Servomotores SG90
Encargado de dispensar la patilla con un movimiento controlado por el ángulo.

<img width="200" height="200" alt="image" src="https://github.com/user-attachments/assets/0e73dde1-2a58-40f7-b3f6-a2844235491b" />

#### -Nextion
Es la interfaz de conexión con el usuario y se comunica por UART, además, permite la configuración de horarios, visualización de estados y notificación de errores.

<img width="200" height="200" alt="image" src="https://github.com/user-attachments/assets/b9b684d5-14ab-4667-8546-f5503b04f1b0" />

#### -Buzzer
Funciona para generar alertas auditivas durante eventos críticos como dispensación o errores.
El buzzer es controlado directamente por el ESP32

<img width="200" height="200" alt="image" src="https://github.com/user-attachments/assets/4708044c-3c82-445e-8a5d-6eb6ef8f009d" />

#### - LEDs indicadores
Se implementan LEDs indicadores para proporcionar retroalimentación visual del estado del sistema.

<img width="200" height="200" alt="image" src="https://github.com/user-attachments/assets/a58780c4-8caa-49fe-a3c1-d26eb0eec8cf" />

#### -Fuente de alimentación externa
El sistema se alimenta mediante una fuente de 5V DC.

<img width="200" height="200" alt="image" src="https://github.com/user-attachments/assets/fc3fc6ea-72e2-48df-bdab-141b64d8c6f0" />



