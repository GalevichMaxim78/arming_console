#include <stdio.h>
#include "load_driver.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/ledc.h"

static const char *TAG = "load_driver";

// Багато готових релейних модулів (як-от Songle-плати із власним драйвером)
// насправді вмикають реле подачею LOW на IN, а не HIGH — попри те, що
// "логічно" очікуєш протилежне. Перевірте свій модуль (ручний тест: IN на
// GND напряму, в обхід ESP32) і виставте це значення відповідно — решта
// коду інвертувати не потрібно.
#define LOAD_RELAY_ACTIVE_LOW 1

#define BUZZER_LEDC_TIMER   LEDC_TIMER_0
#define BUZZER_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZER_LEDC_FREQ_HZ 2000// чутний тон для п'єзо-зумера
#define BUZZER_LEDC_RES     LEDC_TIMER_8_BIT// 0..255 кроків шпаруватості -> гучність
static uint8_t s_buzzer_volume_percent = 60;

void load_driver_init(void)
{
    gpio_config_t cfg =
    {
        .pin_bit_mask = (1ULL << LOAD_RELAY_PIN), // у режимі TRANSISTOR він конфігурується нижче через ledc, не тут
        .mode= GPIO_MODE_OUTPUT,
        .pull_up_en= GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&cfg);
    load_relay_set(false); // через абстракцію, а не сирий рівень — тоді ACTIVE_LOW враховується і тут

    ledc_timer_config_t timer_cfg =
    {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_LEDC_RES,
        .timer_num= BUZZER_LEDC_TIMER,
        .freq_hz= BUZZER_LEDC_FREQ_HZ,
        .clk_cfg= LEDC_AUTO_CLK,
    };

    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t channel_cfg =
    {
        .gpio_num = LOAD_BUZZER_PIN,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel= BUZZER_LEDC_CHANNEL,
        .timer_sel= BUZZER_LEDC_TIMER,
        .duty= 0,
        .hpoint= 0,
    };

    ledc_channel_config(&channel_cfg);
}

void load_relay_set(bool on)
{
    #if LOAD_RELAY_ACTIVE_LOW
        int level = on ? 0 : 1;
    #else
        int level = on ? 1 : 0;
    #endif

    gpio_set_level(LOAD_RELAY_PIN, level);
    ESP_LOGI(TAG, "RELAY GPIO%d -> %d (logical %s)", LOAD_RELAY_PIN, level, on ? "ON" : "OFF");
}

void load_buzzer_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_buzzer_volume_percent = percent;
}

void load_buzzer_set(bool on)
{
    uint32_t max_duty = (1u << BUZZER_LEDC_RES) - 1u; // 255 для 8-бітної шпаруватості
    uint32_t duty = on ? (max_duty * s_buzzer_volume_percent) / 100u : 0u;
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, duty);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);

    ESP_LOGI(TAG, "BUZZER GPIO%d PWM duty=%lu/%lu (volume=%u%%, %s)",LOAD_BUZZER_PIN, (unsigned long)duty, (unsigned long)max_duty,
            s_buzzer_volume_percent, on ? "ON" : "OFF");
}

void load_buzzer_chirp(uint32_t ms)
{
    // Короткий блокуючий сигнал ДОПУСТИМИЙ лише для миттєвого "біп" (10-200 мс)
    // із контексту звичайної задачі (не ISR, не таймерний колбек) — Модуль 2.2/2.5.
    load_buzzer_set(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    load_buzzer_set(false);
}