#include <stdio.h>
#include "button_fsm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define BTN_FSM_TICK_MS 10
#define BTN_DEBOUNCE_MS 50
#define BTN_LONG_PRESS_MS 2000

static const char *TAG = "panic_isr";
static QueueHandle_t s_panic_queue = NULL;

static void IRAM_ATTR panic_isr_handler(void *arg)
{
    uint32_t dummy = 1;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_panic_queue, &dummy, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void panic_task(void *arg)
{
    void (*callback)(void) = (void (*)(void))arg;
    uint32_t dummy;
    while (1)
    {
        if (xQueueReceive(s_panic_queue, &dummy, portMAX_DELAY))
        {
            ESP_LOGE(TAG, "!!! PANIC — апаратне переривання GPIO%d !!!", PANIC_PIN);
            if (callback)
            {
                callback();
            }
        }
    }
}

void panic_sensor_init(void (*on_panic_deferred)(void))
{
    gpio_config_t cfg =
    {
        .pin_bit_mask = (1ULL << PANIC_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en= GPIO_PULLUP_DISABLE,
        .intr_type= GPIO_INTR_POSEDGE,
    };

    gpio_config(&cfg);
    s_panic_queue = xQueueCreate(4, sizeof(uint32_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PANIC_PIN, panic_isr_handler, NULL);
    
    // Пріоритет 15 — НАЙВИЩИЙ у системі: тривога має "перебити" навіть FSM-тік і АЦП
    xTaskCreate(panic_task, "panic_task", 3072, (void *)on_panic_deferred, 15, NULL);
}

static void fsm_timer_cb(void *arg) 
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    app_fsm_tick_all(now_ms);
}

void button_fsm_start_timer(void)
{
    const esp_timer_create_args_t args =
    {
         .callback = &fsm_timer_cb,
         .name = "fsm_tick"
    };
    
    esp_timer_handle_t timer;
    esp_timer_create(&args, &timer);
    esp_timer_start_periodic(timer, BTN_FSM_TICK_MS * 1000ULL);
}

static inline bool raw_pressed(gpio_num_t pin)
{
    return gpio_get_level(pin) == 1;
}

void button_fsm_init(button_fsm_t *btn, gpio_num_t pin)
{
    gpio_config_t cfg =
    {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en= GPIO_PULLUP_DISABLE,
        .intr_type= GPIO_INTR_DISABLE,
    };

    gpio_config(&cfg);
    btn->pin = pin;
    btn->state = BTN_STATE_IDLE;
    btn->state_enter_ms = 0;
    btn->long_fired = false;
}

button_event_t button_fsm_tick(button_fsm_t *btn, uint32_t now_ms)
{
    bool pressed = raw_pressed(btn->pin);
    button_event_t event = BTN_EVENT_NONE;
    switch (btn->state)
    {
        case BTN_STATE_IDLE:
            if (pressed)
            { 
                btn->state = BTN_STATE_PRESSED;
                btn->state_enter_ms = now_ms;
            }
            break;

        case BTN_STATE_PRESSED:
            if (!pressed)
            {
                btn->state = BTN_STATE_IDLE;
            }
            else if (now_ms - btn->state_enter_ms >= BTN_DEBOUNCE_MS)
            {
                btn->state = BTN_STATE_HELD;
                btn->state_enter_ms = now_ms;
                btn->long_fired = false;
            }
            break;

        case BTN_STATE_HELD:
        {
            uint32_t held_ms = now_ms - btn->state_enter_ms;
            if (!pressed)
            {
                if (!btn->long_fired) event = BTN_EVENT_SHORT_PRESS;
                btn->state = BTN_STATE_RELEASE;
                btn->state_enter_ms = now_ms;
            }
            else if (!btn->long_fired && held_ms >= BTN_LONG_PRESS_MS)
            {
                event = BTN_EVENT_LONG_PRESS;
                btn->long_fired = true;
            }
            break;
        }

        case BTN_STATE_RELEASE:
            if (pressed)
            {
                btn->state = BTN_STATE_HELD;
            }
            else if (now_ms - btn->state_enter_ms >= BTN_DEBOUNCE_MS)
            {
                btn->state = BTN_STATE_IDLE;
            }
            break;
    }
    return event;
}