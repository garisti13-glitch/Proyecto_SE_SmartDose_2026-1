#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
 
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "driver/uart.h"
 
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
 
// -----------------------------------------------------------------------------
// PINES
// -----------------------------------------------------------------------------
 
#define SERVO_GPIO_T1   14
#define SERVO_GPIO_T2   27
#define SERVO_GPIO_T3   16
#define SERVO_GPIO_T4   17
 
// Solo sensores de bandeja. Los sensores superiores fueron eliminados.
#define IR_BAND_T1      36
#define IR_BAND_T2      39
#define IR_BAND_T3      25
#define IR_BAND_T4      26
 
#define PIN_BUZZER      13
#define PIN_LED         12
 
#define PIN_SDA         21
#define PIN_SCL         22
#define DS3231_ADDR     0x68
#define I2C_PORT        I2C_NUM_0
 
#define UART_NUM_CFG    UART_NUM_0
#define UART_BAUD_CFG   115200
#define UART_BUF        512
 
#define UART_NUM_NEXTION UART_NUM_1
#define NEXTION_TX_GPIO  5   // ESP32 TX -> Nextion RX
#define NEXTION_RX_GPIO  4   // ESP32 RX <- Nextion TX
#define NEXTION_BAUD     9600
#define NEXTION_BUF      512
 
// -----------------------------------------------------------------------------
// PARAMETROS
// -----------------------------------------------------------------------------
 
#define MAX_TUBOS               4
#define SERVO_FREQ_HZ           50
#define SERVO_MIN_US            600
#define SERVO_MAX_US            2200
#define SERVO_ANGULO_ABIERTO    90
#define SERVO_ANGULO_CERRADO    0
#define SERVO_MS_ACCION         400
#define TIMEOUT_CAIDA_MS        3000
#define TIMEOUT_RECOGIDA_MS     60000
#define ERROR_ALARMA_MS         15000
#define MAX_REINTENTOS_CAIDA    4
#define DEBOUNCE_COUNT          3
#define TICK_SCHEDULER_MS       1000
#define TICK_MONITOR_MS         7000
#define TICK_NEXTION_MS         1500
#define MAX_LOGS                5
#define LOG_LEN                 120
 
// FC-51 tipico: LOW = objeto detectado. Si tu sensor trabaja al reves, pon 0.
#define SENSOR_ACTIVO_EN_LOW    1
 
// -----------------------------------------------------------------------------
// ESTRUCTURAS
// -----------------------------------------------------------------------------
 
typedef enum {
    TUBO_OK = 0,
    TUBO_DISPENSANDO,
    TUBO_FALLO,
} estado_tubo_t;
 
typedef struct {
    bool activo;
    int  hora;
    int  minuto;
    int  cantidad;
} horario_t;
 
typedef struct {
    estado_tubo_t estado;
    bool          pastilla_en_bandeja;
    bool          alerta_activa;   // true para dosis lista o error no confirmado
} info_tubo_t;
 
// -----------------------------------------------------------------------------
// VARIABLES GLOBALES
// -----------------------------------------------------------------------------
 
static const char *TAG = "SMARTDOSE";
 
static const int SERVO_GPIOS[MAX_TUBOS] = {
    SERVO_GPIO_T1, SERVO_GPIO_T2, SERVO_GPIO_T3, SERVO_GPIO_T4
};
 
static const int IR_BAND_GPIOS[MAX_TUBOS] = {
    IR_BAND_T1, IR_BAND_T2, IR_BAND_T3, IR_BAND_T4
};
 
static horario_t   g_horarios[MAX_TUBOS];
static info_tubo_t g_info[MAX_TUBOS];
static SemaphoreHandle_t g_mutex;
static int g_ultimo_minuto[MAX_TUBOS];
static char g_logs[MAX_LOGS][LOG_LEN];
 
// -----------------------------------------------------------------------------
// UTILIDADES
// -----------------------------------------------------------------------------
 
static int64_t millis(void)
{
    return esp_timer_get_time() / 1000;
}
 
// -----------------------------------------------------------------------------
// SERVO
// -----------------------------------------------------------------------------
 
static uint32_t angulo_a_duty(int angulo)
{
    if (angulo < 0) angulo = 0;
    if (angulo > 180) angulo = 180;
    uint32_t us = SERVO_MIN_US + (uint32_t)((SERVO_MAX_US - SERVO_MIN_US) * angulo / 180);
    return (us * 16383U) / 20000U;
}
 
static void servo_set(int idx, int angulo)
{
    if (idx < 0 || idx >= MAX_TUBOS) return;
    ledc_channel_t ch = (ledc_channel_t)(LEDC_CHANNEL_0 + idx);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, ch, angulo_a_duty(angulo));
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, ch);
}
 
static void servo_dispensar(int idx, int cantidad)
{
    if (cantidad < 1) cantidad = 1;
    if (cantidad > 10) cantidad = 10;
 
    for (int v = 0; v < cantidad; v++) {
        servo_set(idx, SERVO_ANGULO_ABIERTO);
        vTaskDelay(pdMS_TO_TICKS(SERVO_MS_ACCION));
        servo_set(idx, SERVO_ANGULO_CERRADO);
        vTaskDelay(pdMS_TO_TICKS(SERVO_MS_ACCION));
    }
}
 
// -----------------------------------------------------------------------------
// SENSOR DE BANDEJA
// -----------------------------------------------------------------------------
 
static bool sensor_leer(int gpio)
{
    int detectado = 0;
    for (int i = 0; i < DEBOUNCE_COUNT; i++) {
        int level = gpio_get_level(gpio);
#if SENSOR_ACTIVO_EN_LOW
        if (level == 0) detectado++;
#else
        if (level == 1) detectado++;
#endif
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return detectado > (DEBOUNCE_COUNT / 2);
}
 
static bool bandeja_tiene_pastilla(int idx)
{
    if (idx < 0 || idx >= MAX_TUBOS) return false;
    return sensor_leer(IR_BAND_GPIOS[idx]);
}
 
// -----------------------------------------------------------------------------
// ALERTAS
// -----------------------------------------------------------------------------
 
static void buzzer_set(bool on) { gpio_set_level(PIN_BUZZER, on ? 1 : 0); }
static void led_set(bool on)    { gpio_set_level(PIN_LED,    on ? 1 : 0); }
 
// -----------------------------------------------------------------------------
// RTC DS3231
// -----------------------------------------------------------------------------
 
static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }
 
static bool ds3231_set_hora(int h, int m, int s)
{
    uint8_t data[3];
    data[0] = dec2bcd((uint8_t)s);
    data[1] = dec2bcd((uint8_t)m);
    data[2] = dec2bcd((uint8_t)h);
 
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x00, true);
    i2c_master_write(cmd, data, 3, true);
    i2c_master_stop(cmd);
 
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}
 
static bool ds3231_leer(uint8_t reg, uint8_t *buf, int len)
{
    if (!buf || len <= 0) return false;
 
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &buf[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
 
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}
 
static bool ds3231_get_hora(int *h, int *m, int *s)
{
    uint8_t buf[3];
    if (!ds3231_leer(0x00, buf, 3)) return false;
    if (s) *s = bcd2dec(buf[0] & 0x7F);
    if (m) *m = bcd2dec(buf[1] & 0x7F);
    if (h) *h = bcd2dec(buf[2] & 0x3F);
    return true;
}
 
// -----------------------------------------------------------------------------
// NEXTION
// -----------------------------------------------------------------------------
 
static void nextion_write_raw(const char *s)
{
    if (!s) return;
    uart_write_bytes(UART_NUM_NEXTION, s, strlen(s));
    const uint8_t end[3] = {0xFF, 0xFF, 0xFF};
    uart_write_bytes(UART_NUM_NEXTION, (const char *)end, 3);
}
 
static void nextion_cmd(const char *fmt, ...)
{
    char cmd[180];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    nextion_write_raw(cmd);
}
 
static void nextion_set_txt(const char *obj, const char *txt)
{
    if (!obj || !txt) return;
    char safe[120];
    size_t k = 0;
    for (size_t i = 0; txt[i] && k < sizeof(safe) - 1; i++) {
        if (txt[i] == '"') continue;
        safe[k++] = txt[i];
    }
    safe[k] = '\0';
    nextion_cmd("%s.txt=\"%s\"", obj, safe);
}
 
static void nextion_set_val(const char *obj, int value)
{
    if (!obj) return;
    nextion_cmd("%s.val=%d", obj, value);
}
 
static const char *estado_a_texto(estado_tubo_t e)
{
    switch (e) {
        case TUBO_OK:          return "OK";
        case TUBO_DISPENSANDO: return "DISPENSANDO";
        case TUBO_FALLO:       return "FALLO";
        default:               return "?";
    }
}
 
static void nextion_actualizar_logs(void)
{
    char obj[16];
 
    for (int i = 0; i < MAX_LOGS; i++) {
        snprintf(obj, sizeof(obj), "tLog%d", i + 1);
        if (g_logs[i][0] == '\0') {
            nextion_set_txt(obj, "Sin eventos");
        } else {
            nextion_set_txt(obj, g_logs[i]);
        }
    }
}
 
static void nextion_actualizar_campos_config(void)
{
    for (int i = 0; i < MAX_TUBOS; i++) {
        horario_t hor;
 
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        hor = g_horarios[i];
        xSemaphoreGive(g_mutex);
 
        int h = hor.activo ? hor.hora : 0;
        int m = hor.activo ? hor.minuto : 0;
        int c = hor.activo ? hor.cantidad : 1;
 
        nextion_cmd("nT%dH.val=%d", i + 1, h);
        nextion_cmd("nT%dM.val=%d", i + 1, m);
        nextion_cmd("nT%dC.val=%d", i + 1, c);
    }
}
 
static void nextion_limpiar_campos_config(void)
{
    for (int i = 0; i < MAX_TUBOS; i++) {
        nextion_cmd("nT%dH.val=0", i + 1);
        nextion_cmd("nT%dM.val=0", i + 1);
        nextion_cmd("nT%dC.val=1", i + 1);
    }
}
 
static void nextion_actualizar_pantalla(void)
{
    int h = 0, m = 0, s = 0;
    bool rtc_ok = ds3231_get_hora(&h, &m, &s);
 
    char txt[120];
    if (rtc_ok) snprintf(txt, sizeof(txt), "%02d:%02d:%02d", h, m, s);
    else        snprintf(txt, sizeof(txt), "RTC ERROR");
    nextion_set_txt("tHora", txt);
 
    bool alerta_general = false;
    bool fallo_general = false;
 
    for (int i = 0; i < MAX_TUBOS; i++) {
        horario_t hor;
        info_tubo_t info;
 
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        hor = g_horarios[i];
        info = g_info[i];
        xSemaphoreGive(g_mutex);
 
        if (info.alerta_activa) alerta_general = true;
        if (info.estado == TUBO_FALLO) fallo_general = true;
 
        if (hor.activo) snprintf(txt, sizeof(txt), "%02d:%02d x%d", hor.hora, hor.minuto, hor.cantidad);
        else            snprintf(txt, sizeof(txt), "Sin horario");
 
        char obj[20];
        snprintf(obj, sizeof(obj), "tT%dHor", i + 1);
        nextion_set_txt(obj, txt);
    }
 
    if (fallo_general)       nextion_set_txt("tMsg", "Error registrado. Revise mecanismo.");
    else if (alerta_general) nextion_set_txt("tMsg", "Dosis lista. Retirar pastilla.");
    else                    nextion_set_txt("tMsg", "SmartDose listo.");
 
    nextion_set_val("jAlerta", alerta_general ? 100 : 0);
    nextion_actualizar_logs();
}
 
// -----------------------------------------------------------------------------
// NVS
// -----------------------------------------------------------------------------
 
static void nvs_guardar(void)
{
    char buf[256];
    buf[0] = '\0';
    bool hay = false;
 
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TUBOS; i++) {
        if (!g_horarios[i].activo) continue;
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "%sT%d=%02d:%02d:%d",
                 hay ? ";" : "", i + 1,
                 g_horarios[i].hora, g_horarios[i].minuto, g_horarios[i].cantidad);
        strlcat(buf, tmp, sizeof(buf));
        hay = true;
    }
    xSemaphoreGive(g_mutex);
 
    if (!hay) strlcpy(buf, "VACIO", sizeof(buf));
 
    nvs_handle_t hdl;
    if (nvs_open("smartdose", NVS_READWRITE, &hdl) != ESP_OK) return;
    nvs_set_str(hdl, "horarios", buf);
    nvs_commit(hdl);
    nvs_close(hdl);
    ESP_LOGI(TAG, "Guardado en NVS: %s", buf);
}
 
static void nvs_cargar(void)
{
    nvs_handle_t hdl;
    if (nvs_open("smartdose", NVS_READONLY, &hdl) != ESP_OK) return;
 
    char buf[256];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(hdl, "horarios", buf, &len);
    nvs_close(hdl);
 
    if (err != ESP_OK || strcmp(buf, "VACIO") == 0) return;
 
    char *sp = NULL;
    char *tok = strtok_r(buf, ";", &sp);
    while (tok) {
        int t = 0, h = 0, m = 0, c = 1;
        if (sscanf(tok, "T%d=%d:%d:%d", &t, &h, &m, &c) == 4 &&
            t >= 1 && t <= MAX_TUBOS && h >= 0 && h <= 23 &&
            m >= 0 && m <= 59 && c >= 1 && c <= 10) {
            g_horarios[t - 1].activo = true;
            g_horarios[t - 1].hora = h;
            g_horarios[t - 1].minuto = m;
            g_horarios[t - 1].cantidad = c;
            ESP_LOGI(TAG, "Cargado: Tubo %d -> %02d:%02d x%d", t, h, m, c);
        }
        tok = strtok_r(NULL, ";", &sp);
    }
}
 
static bool rtc_ya_configurado(void)
{
    nvs_handle_t hdl;
    if (nvs_open("smartdose", NVS_READONLY, &hdl) != ESP_OK) return false;
 
    uint8_t flag = 0;
    esp_err_t err = nvs_get_u8(hdl, "rtc_set", &flag);
    nvs_close(hdl);
    return (err == ESP_OK && flag == 1);
}
 
static void marcar_rtc_configurado(void)
{
    nvs_handle_t hdl;
    if (nvs_open("smartdose", NVS_READWRITE, &hdl) != ESP_OK) return;
 
    nvs_set_u8(hdl, "rtc_set", 1);
    nvs_commit(hdl);
    nvs_close(hdl);
}
 
// -----------------------------------------------------------------------------
// LOG
// -----------------------------------------------------------------------------
 
static void smartdose_log(int tubo, const char *nivel, const char *codigo, const char *msg)
{
    int h = 0, m = 0, s = 0;
    ds3231_get_hora(&h, &m, &s);
 
    char linea[LOG_LEN];
 
    if (tubo >= 0 && tubo < MAX_TUBOS) {
        if (codigo && codigo[0] != '\0') {
            snprintf(linea, sizeof(linea), "%02d:%02d:%02d | %s %s | T%d | %s",
                     h, m, s, nivel, codigo, tubo + 1, msg);
        } else {
            snprintf(linea, sizeof(linea), "%02d:%02d:%02d | %s | T%d | %s",
                     h, m, s, nivel, tubo + 1, msg);
        }
    } else {
        if (codigo && codigo[0] != '\0') {
            snprintf(linea, sizeof(linea), "%02d:%02d:%02d | %s %s | SYS | %s",
                     h, m, s, nivel, codigo, msg);
        } else {
            snprintf(linea, sizeof(linea), "%02d:%02d:%02d | %s | SYS | %s",
                     h, m, s, nivel, msg);
        }
    }
 
    printf("%s\n", linea);
 
    for (int i = MAX_LOGS - 1; i > 0; i--) {
        strlcpy(g_logs[i], g_logs[i - 1], sizeof(g_logs[i]));
    }
    strlcpy(g_logs[0], linea, sizeof(g_logs[0]));
 
    nextion_actualizar_logs();
 
    char resumen[120];
    if (tubo >= 0 && tubo < MAX_TUBOS) {
        snprintf(resumen, sizeof(resumen), "Tubo %d: %s", tubo + 1, msg);
    } else {
        snprintf(resumen, sizeof(resumen), "%s", msg);
    }
    nextion_set_txt("tMsg", resumen);
}
 
static void log_evento(int tubo, const char *nivel, const char *msg)
{
    smartdose_log(tubo, nivel, "", msg);
}
 
static void log_error_01(int tubo)
{
    smartdose_log(tubo, "ERROR", "01", "No se detecto pastilla tras 4 intentos");
}
 
// -----------------------------------------------------------------------------
// DISPENSACION
// -----------------------------------------------------------------------------
 
static bool hay_alguna_alerta_activa(void)
{
    bool hay = false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TUBOS; i++) {
        if (g_info[i].alerta_activa || g_info[i].pastilla_en_bandeja) {
            hay = true;
            break;
        }
    }
    xSemaphoreGive(g_mutex);
    return hay;
}
 
static void actualizar_salidas_alarma(void)
{
    if (hay_alguna_alerta_activa()) {
        led_set(true);
        buzzer_set(true);
    } else {
        led_set(false);
        buzzer_set(false);
    }
}
 
static bool dispensar_con_reintentos_y_confirmar_bandeja(int idx, int cantidad)
{
    if (cantidad < 1) cantidad = 1;
    if (cantidad > 10) cantidad = 10;
 
    for (int intento = 1; intento <= MAX_REINTENTOS_CAIDA; intento++) {
        char msg[120];
        snprintf(msg, sizeof(msg), "Tubo %d intento %d/%d", idx + 1, intento, MAX_REINTENTOS_CAIDA);
        log_evento(idx, "INFO", msg);
        nextion_set_txt("tMsg", msg);
 
        servo_dispensar(idx, cantidad);
 
        int64_t t0 = millis();
        while ((millis() - t0) < TIMEOUT_CAIDA_MS) {
            if (bandeja_tiene_pastilla(idx)) {
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
 
        char warn_msg[120];
        snprintf(warn_msg, sizeof(warn_msg), "No se detecto pastilla. Reintento %d/%d",
                 intento, MAX_REINTENTOS_CAIDA);
        log_evento(idx, "WARN", warn_msg);
        nextion_set_txt("tMsg", "No cayo dosis. Reintentando...");
    }
 
    return false;
}
 
static void ciclo_dispensacion(int idx)
{
    if (idx < 0 || idx >= MAX_TUBOS) return;
 
    int cantidad = 1;
 
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_info[idx].estado == TUBO_DISPENSANDO) {
        xSemaphoreGive(g_mutex);
        return;
    }
    cantidad = g_horarios[idx].cantidad;
    if (cantidad < 1) cantidad = 1;
    if (cantidad > 10) cantidad = 10;
    g_info[idx].estado = TUBO_DISPENSANDO;
    g_info[idx].alerta_activa = false;
    g_info[idx].pastilla_en_bandeja = false;
    xSemaphoreGive(g_mutex);
 
    log_evento(idx, "INFO", "Iniciando dispensacion");
    nextion_cmd("page page0");
    nextion_set_txt("tMsg", "Dispensando dosis...");
    nextion_actualizar_pantalla();
 
    bool cayo = dispensar_con_reintentos_y_confirmar_bandeja(idx, cantidad);
 
    if (!cayo) {
        char msg[120];
        snprintf(msg, sizeof(msg), "ERROR 01: Tubo %d no dispenso. Revisar atasco o recargar.", idx + 1);
        log_error_01(idx);
 
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_info[idx].estado = TUBO_FALLO;
        g_info[idx].pastilla_en_bandeja = false;
        g_info[idx].alerta_activa = true;
        xSemaphoreGive(g_mutex);
 
        nextion_cmd("page page3");
        nextion_set_txt("tErrorMsg", msg);
        nextion_set_txt("tMsg", msg);
        actualizar_salidas_alarma();
        nextion_actualizar_pantalla();
 
        // Caso error: LED + buzzer solo durante 15 segundos.
        vTaskDelay(pdMS_TO_TICKS(ERROR_ALARMA_MS));
 
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        // Mantiene el estado FALLO para el log/status, pero apaga la alarma sonora/visual.
        if (g_info[idx].estado == TUBO_FALLO) {
            g_info[idx].alerta_activa = false;
            g_info[idx].pastilla_en_bandeja = false;
        }
        xSemaphoreGive(g_mutex);
 
        buzzer_set(false);
        led_set(false);
        nextion_set_txt("tMsg", "Error registrado. Revisar mecanismo.");
        nextion_actualizar_pantalla();
        return;
    }
 
    log_evento(idx, "WARN", "Dosis en bandeja, esperando retiro");
 
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_info[idx].estado = TUBO_OK;
    g_info[idx].pastilla_en_bandeja = true;
    g_info[idx].alerta_activa = true;
    xSemaphoreGive(g_mutex);
 
    nextion_cmd("page page2");
    nextion_set_txt("tAlertMsg", "Retira la pastilla de la bandeja");
    nextion_set_txt("tMsg", "Dosis lista. Retirar pastilla.");
    actualizar_salidas_alarma();
    nextion_actualizar_pantalla();
 
    int64_t t0 = millis();
    while (1) {
        bool sigue_en_bandeja = bandeja_tiene_pastilla(idx);
 
        if (!sigue_en_bandeja) {
            // Confirmacion corta para evitar apagar por ruido momentaneo del sensor.
            vTaskDelay(pdMS_TO_TICKS(300));
            if (!bandeja_tiene_pastilla(idx)) {
                break;
            }
        }
 
        if ((millis() - t0) >= TIMEOUT_RECOGIDA_MS) {
            log_evento(idx, "WARN", "Pastilla no recogida");
            t0 = millis();
        }
        actualizar_salidas_alarma();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
 
    // Apagado inmediato al confirmar que la bandeja quedo vacia.
    buzzer_set(false);
    led_set(false);
 
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_info[idx].pastilla_en_bandeja = false;
    g_info[idx].alerta_activa = false;
    g_info[idx].estado = TUBO_OK;
    xSemaphoreGive(g_mutex);
 
    actualizar_salidas_alarma();
    nextion_cmd("page page0");
    log_evento(idx, "INFO", "Pastilla retirada. Ciclo completado");
    nextion_actualizar_pantalla();
}
 
static void tarea_dispensacion(void *p)
{
    int idx = (int)(uintptr_t)p;
    ciclo_dispensacion(idx);
    vTaskDelete(NULL);
}
 
static bool solicitar_dispensacion(int idx)
{
    if (idx < 0 || idx >= MAX_TUBOS) return false;
 
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bool ocupado = (g_info[idx].estado == TUBO_DISPENSANDO);
    xSemaphoreGive(g_mutex);
 
    if (ocupado) {
        printf("[WARN] Tubo %d ya esta dispensando.\n", idx + 1);
        return false;
    }
 
    BaseType_t ok = xTaskCreate(
        tarea_dispensacion,
        "dispensacion",
        4096,
        (void *)(uintptr_t)idx,
        7,
        NULL
    );
    return ok == pdPASS;
}
 
// -----------------------------------------------------------------------------
// COMANDOS
// -----------------------------------------------------------------------------
 
static void imprimir_status(void)
{
    int h = 0, m = 0, s = 0;
    bool rtc_ok = ds3231_get_hora(&h, &m, &s);
 
    printf("\n=== SMARTDOSE STATUS ===\n");
    if (rtc_ok) printf("Hora RTC: %02d:%02d:%02d\n", h, m, s);
    else        printf("Hora RTC: ERROR\n");
 
    for (int i = 0; i < MAX_TUBOS; i++) {
        horario_t hor;
        info_tubo_t info;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        hor = g_horarios[i];
        info = g_info[i];
        xSemaphoreGive(g_mutex);
 
        printf("Tubo %d | estado=%s | almacenamiento=NO_EVALUADO | bandeja=%s | alerta=%s | horario=",
               i + 1,
               estado_a_texto(info.estado),
               bandeja_tiene_pastilla(i) ? "SI" : "NO",
               info.alerta_activa ? "SI" : "NO");
 
        if (hor.activo) printf("%02d:%02d x%d\n", hor.hora, hor.minuto, hor.cantidad);
        else            printf("SIN HORARIO\n");
    }
    printf("========================\n\n");
}
 
static void limpiar_y_normalizar(const char *cmd, char *out, size_t out_sz)
{
    size_t k = 0;
    for (size_t i = 0; cmd && cmd[i] && k < out_sz - 1; i++) {
        char c = cmd[i];
        if (c == '\r' || c == '\n' || c == (char)0xFF) continue;
        if (!isspace((unsigned char)c)) out[k++] = (char)tolower((unsigned char)c);
    }
    out[k] = '\0';
}
 
static bool configurar_horario(int tubo, int h, int m, int cantidad)
{
    if (tubo < 1 || tubo > MAX_TUBOS || h < 0 || h > 23 ||
        m < 0 || m > 59 || cantidad < 1 || cantidad > 10) {
        printf("[ERROR] Rango invalido: tubo 1-4, hora 0-23, min 0-59, cantidad 1-10.\n");
        nextion_set_txt("tMsg", "Valores fuera de rango");
        return false;
    }
 
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_horarios[tubo - 1].activo = true;
    g_horarios[tubo - 1].hora = h;
    g_horarios[tubo - 1].minuto = m;
    g_horarios[tubo - 1].cantidad = cantidad;
    g_ultimo_minuto[tubo - 1] = -1;
    xSemaphoreGive(g_mutex);
 
    nvs_guardar();
    printf("[OK] Tubo %d -> %02d:%02d x%d\n", tubo, h, m, cantidad);
 
    char msg[120];
    snprintf(msg, sizeof(msg), "Horario guardado T%d %02d:%02d x%d", tubo, h, m, cantidad);
    log_evento(tubo - 1, "INFO", msg);
 
    nextion_set_txt("tMsg", "Horario guardado.");
    nextion_actualizar_campos_config();
    nextion_actualizar_pantalla();
    return true;
}
 
static void ack_tubo(int idx)
{
    if (idx < 0 || idx >= MAX_TUBOS) return;
 
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_info[idx].alerta_activa = false;
    g_info[idx].pastilla_en_bandeja = false;
    if (g_info[idx].estado == TUBO_FALLO) g_info[idx].estado = TUBO_OK;
    xSemaphoreGive(g_mutex);
 
    actualizar_salidas_alarma();
    printf("[OK] Alerta confirmada tubo %d.\n", idx + 1);
    nextion_cmd("page page0");
    nextion_actualizar_pantalla();
}
 
static void procesar_comando(const char *cmd)
{
    char limpio[256];
    limpiar_y_normalizar(cmd, limpio, sizeof(limpio));
    if (limpio[0] == '\0') return;
 
    if (strncmp(limpio, "nxt:", 4) == 0) {
        memmove(limpio, limpio + 4, strlen(limpio + 4) + 1);
    }
 
    if (strncmp(limpio, "settime=", 8) == 0) {
        int h = 0, m = 0, s = 0;
        if (sscanf(limpio + 8, "%d:%d:%d", &h, &m, &s) == 3 &&
            h >= 0 && h <= 23 && m >= 0 && m <= 59 && s >= 0 && s <= 59) {
            if (ds3231_set_hora(h, m, s)) {
                marcar_rtc_configurado();
                printf("[OK] Hora actualizada a %02d:%02d:%02d\n", h, m, s);
                nextion_set_txt("tMsg", "Hora actualizada");
                nextion_actualizar_pantalla();
            } else {
                printf("[ERROR] No se pudo actualizar RTC\n");
            }
        } else {
            printf("[ERROR] Formato correcto: SETTIME=HH:MM:SS\n");
        }
        return;
    }
 
    if (strncmp(limpio, "cfg,", 4) == 0) {
        int tubo = 0, h = 0, m = 0, cantidad = 1;
        if (sscanf(limpio, "cfg,%d,%d,%d,%d", &tubo, &h, &m, &cantidad) == 4) {
            configurar_horario(tubo, h, m, cantidad);
        } else {
            printf("[ERROR] Formato correcto: CFG,tubo,hora,min,cantidad\n");
            nextion_set_txt("tMsg", "Formato CFG invalido");
        }
        return;
    }
 
    if (strcmp(limpio, "status") == 0 || strcmp(limpio, "refresh") == 0) {
        imprimir_status();
        nextion_actualizar_campos_config();
        nextion_actualizar_pantalla();
        return;
    }
 
    if (strcmp(limpio, "clear") == 0 || strcmp(limpio, "borrar") == 0) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_TUBOS; i++) {
            g_horarios[i].activo = false;
            g_horarios[i].hora = 0;
            g_horarios[i].minuto = 0;
            g_horarios[i].cantidad = 1;
            g_ultimo_minuto[i] = -1;
            g_info[i].estado = TUBO_OK;
            g_info[i].alerta_activa = false;
            g_info[i].pastilla_en_bandeja = false;
        }
        xSemaphoreGive(g_mutex);
        actualizar_salidas_alarma();
        nvs_guardar();
        printf("[OK] Todos los horarios borrados.\n");
        nextion_set_txt("tMsg", "Horarios borrados.");
        nextion_limpiar_campos_config();
        log_evento(-1, "INFO", "Horarios borrados");
        nextion_actualizar_pantalla();
        return;
    }
 
    if (strncmp(limpio, "disp", 4) == 0 && isdigit((unsigned char)limpio[4])) {
        int tubo = atoi(&limpio[4]);
        if (tubo >= 1 && tubo <= MAX_TUBOS) {
            printf("[OK] Dispensacion manual tubo %d.\n", tubo);
            if (!solicitar_dispensacion(tubo - 1)) {
                nextion_set_txt("tMsg", "No se pudo iniciar dispensacion.");
            }
        } else {
            printf("[ERROR] Tubo fuera de rango. Usa DISP1..DISP4.\n");
        }
        return;
    }
 
    if (strncmp(limpio, "ack", 3) == 0 && isdigit((unsigned char)limpio[3])) {
        int tubo = atoi(&limpio[3]);
        if (tubo >= 1 && tubo <= MAX_TUBOS) ack_tubo(tubo - 1);
        else printf("[ERROR] Tubo fuera de rango. Usa ACK1..ACK4.\n");
        return;
    }
 
    // Formato antiguo: T1=08:30:2 o varios separados por ;
    char copia[256];
    strlcpy(copia, limpio, sizeof(copia));
    bool al_menos_uno = false;
 
    char *sp = NULL;
    char *tok = strtok_r(copia, ";", &sp);
    while (tok) {
        char *p = tok;
        if (strncmp(p, "tubo", 4) == 0) p += 4;
        else if (*p == 't') p += 1;
 
        if (!isdigit((unsigned char)*p)) {
            printf("[ERROR] Formato invalido: %s | Uso: T1=08:30:2\n", tok);
            nextion_set_txt("tMsg", "Formato invalido");
            return;
        }
 
        char *ep = NULL;
        int tubo = (int)strtol(p, &ep, 10);
        p = ep;
        while (*p == '=' || *p == ',' || *p == '-') p++;
 
        int h = (int)strtol(p, &ep, 10); p = ep;
        if (*p != ':') {
            printf("[ERROR] Falta ':' entre hora y minuto.\n");
            return;
        }
        p++;
        int m = (int)strtol(p, &ep, 10); p = ep;
        int cantidad = 1;
        if (*p == ':') {
            p++;
            cantidad = (int)strtol(p, &ep, 10);
        }
 
        if (!configurar_horario(tubo, h, m, cantidad)) return;
        al_menos_uno = true;
        tok = strtok_r(NULL, ";", &sp);
    }
 
    if (!al_menos_uno) {
        printf("[ERROR] Ningun comando valido reconocido.\n");
        nextion_set_txt("tMsg", "Comando no reconocido");
    }
}
 
// -----------------------------------------------------------------------------
// TAREAS
// -----------------------------------------------------------------------------
 
static void tarea_scheduler(void *p)
{
    while (1) {
        int h = 0, m = 0, s = 0;
        if (!ds3231_get_hora(&h, &m, &s)) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
 
        for (int i = 0; i < MAX_TUBOS; i++) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            bool activo = g_horarios[i].activo;
            int h_prog = g_horarios[i].hora;
            int m_prog = g_horarios[i].minuto;
            bool dispensando = (g_info[i].estado == TUBO_DISPENSANDO);
            xSemaphoreGive(g_mutex);
 
            if (!activo || dispensando) continue;
 
            if (h != h_prog || m != m_prog) {
                g_ultimo_minuto[i] = -1;
                continue;
            }
 
            if (g_ultimo_minuto[i] == m) continue;
 
            g_ultimo_minuto[i] = m;
            solicitar_dispensacion(i);
        }
 
        vTaskDelay(pdMS_TO_TICKS(TICK_SCHEDULER_MS));
    }
}
 
static void tarea_monitor(void *p)
{
    while (1) {
        int h = 0, m = 0, s = 0;
        ds3231_get_hora(&h, &m, &s);
 
        printf("\n[INFO] %02d:%02d:%02d - Estado tubos:\n", h, m, s);
 
        for (int i = 0; i < MAX_TUBOS; i++) {
            bool band = bandeja_tiene_pastilla(i);
 
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            estado_tubo_t est = g_info[i].estado;
 
            if (est != TUBO_DISPENSANDO) {
                g_info[i].pastilla_en_bandeja = band;
            }
 
            // Caso dosis normal: si ya no hay pastilla en bandeja, se apaga la alerta.
            // No se limpia automaticamente un TUBO_FALLO, porque ese error se apaga
            // por temporizador de 15 segundos o por ACK.
            if (est == TUBO_OK && g_info[i].alerta_activa && !band) {
                g_info[i].alerta_activa = false;
            }
 
            bool alerta = g_info[i].alerta_activa;
            est = g_info[i].estado;
            xSemaphoreGive(g_mutex);
 
            printf("  Tubo %d: %-12s | almacenamiento=NO_EVALUADO | bandeja=%s | alerta=%s\n",
                   i + 1,
                   estado_a_texto(est),
                   band ? "SI" : "NO",
                   alerta ? "SI" : "NO");
        }
 
        actualizar_salidas_alarma();
        vTaskDelay(pdMS_TO_TICKS(TICK_MONITOR_MS));
    }
}
 
static void tarea_uart_monitor(void *p)
{
    uint8_t buf[UART_BUF];
    char linea[256];
    int pos = 0;
 
    printf("\nSmartDose listo. Comandos: T1=08:30:1 | CFG,1,8,30,1 | DISP1 | ACK1 | STATUS | CLEAR | SETTIME=HH:MM:SS\n\n");
 
    while (1) {
        int n = uart_read_bytes(UART_NUM_CFG, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) continue;
 
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            uart_write_bytes(UART_NUM_CFG, &c, 1);
 
            if (c == '\n' || c == '\r') {
                if (pos > 0) {
                    linea[pos] = '\0';
                    printf("\n");
                    procesar_comando(linea);
                    pos = 0;
                }
            } else if (pos < (int)sizeof(linea) - 1) {
                linea[pos++] = c;
            }
        }
    }
}
 
static void tarea_nextion_rx(void *p)
{
    uint8_t buf[NEXTION_BUF];
    char linea[256];
    int pos = 0;
    int ff_count = 0;
 
    while (1) {
        int n = uart_read_bytes(UART_NUM_NEXTION, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) continue;
 
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
 
            if (b == 0xFF) {
                ff_count++;
                if (ff_count >= 3 && pos > 0) {
                    linea[pos] = '\0';
                    printf("[NEXTION] %s\n", linea);
                    procesar_comando(linea);
                    pos = 0;
                    ff_count = 0;
                }
                continue;
            }
            ff_count = 0;
 
            if (b == '\n' || b == '\r') {
                if (pos > 0) {
                    linea[pos] = '\0';
                    printf("[NEXTION] %s\n", linea);
                    procesar_comando(linea);
                    pos = 0;
                }
            } else if (b >= 32 && b <= 126 && pos < (int)sizeof(linea) - 1) {
                linea[pos++] = (char)b;
            }
        }
    }
}
 
static void tarea_nextion_update(void *p)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    nextion_set_txt("tMsg", "SmartDose iniciando...");
 
    while (1) {
        nextion_actualizar_pantalla();
        vTaskDelay(pdMS_TO_TICKS(TICK_NEXTION_MS));
    }
}
 
// -----------------------------------------------------------------------------
// APP MAIN
// -----------------------------------------------------------------------------
 
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
 
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) {
        ESP_LOGE(TAG, "No se pudo crear mutex");
        return;
    }
 
    for (int i = 0; i < MAX_TUBOS; i++) {
        g_horarios[i].activo = false;
        g_horarios[i].hora = 0;
        g_horarios[i].minuto = 0;
        g_horarios[i].cantidad = 1;
        g_info[i].estado = TUBO_OK;
        g_info[i].pastilla_en_bandeja = false;
        g_info[i].alerta_activa = false;
        g_ultimo_minuto[i] = -1;
    }
 
   
    for (int i = 0; i < MAX_LOGS; i++) {
        strlcpy(g_logs[i], "Sin eventos", sizeof(g_logs[i]));
    }
 
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_HIGH_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .freq_hz         = SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
 
    for (int i = 0; i < MAX_TUBOS; i++) {
        ledc_channel_config_t ch = {
            .gpio_num   = SERVO_GPIOS[i],
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .channel    = (ledc_channel_t)(LEDC_CHANNEL_0 + i),
            .timer_sel  = LEDC_TIMER_0,
            .duty       = angulo_a_duty(SERVO_ANGULO_CERRADO),
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
 
    for (int i = 0; i < MAX_TUBOS; i++) {
        gpio_config_t cfg_band = {
            .pin_bit_mask = (1ULL << IR_BAND_GPIOS[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg_band));
    }
 
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_BUZZER) | (1ULL << PIN_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_BUZZER, 0);
    gpio_set_level(PIN_LED, 0);
 
    i2c_config_t i2c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &i2c));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));
 
    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD_CFG,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk  = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_CFG, &uart_cfg));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_CFG, UART_BUF * 2, 0, 0, NULL, 0));
 
    uart_config_t uart_nextion = {
        .baud_rate  = NEXTION_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk  = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_NEXTION, &uart_nextion));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_NEXTION, NEXTION_TX_GPIO, NEXTION_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_NEXTION, NEXTION_BUF * 2, NEXTION_BUF * 2, 0, NULL, 0));
 
    nvs_cargar();
 
    if (!rtc_ya_configurado()) {
        ESP_LOGW(TAG, "RTC no configurado. Inicializando a 17:43:00");
        if (ds3231_set_hora(17, 43, 0)) {
            marcar_rtc_configurado();
            ESP_LOGI(TAG, "RTC configurado correctamente");
        } else {
            ESP_LOGE(TAG, "Error configurando RTC");
        }
    }
 
    int h = 0, m = 0, s = 0;
    if (ds3231_get_hora(&h, &m, &s)) {
        ESP_LOGI(TAG, "DS3231 OK - Hora actual: %02d:%02d:%02d", h, m, s);
    } else {
        ESP_LOGE(TAG, "DS3231 no responde");
    }
 
    for (int i = 0; i < MAX_TUBOS; i++) servo_set(i, SERVO_ANGULO_CERRADO);
 
    xTaskCreate(tarea_scheduler,      "scheduler",      4096, NULL, 6, NULL);
    xTaskCreate(tarea_monitor,        "monitor",        4096, NULL, 4, NULL);
    xTaskCreate(tarea_uart_monitor,   "uart_monitor",   4096, NULL, 3, NULL);
    xTaskCreate(tarea_nextion_rx,     "nextion_rx",     4096, NULL, 5, NULL);
    xTaskCreate(tarea_nextion_update, "nextion_update", 4096, NULL, 2, NULL);
 
    ESP_LOGI(TAG, "=== SmartDose listo con Nextion y solo sensor de bandeja ===");
}
