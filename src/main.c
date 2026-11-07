#include <stdint.h>
#include <stdbool.h>
#include "load_driver.h"
#include "button_fsm.h"
#include "pot_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "arming_console";

// Системна FSM — стан ВСІЄЇ охоронної панелі
typedef enum
{
    SYS_DISARMED = 0,// система знята з охорони, тестування зон дозволене без тривоги
    SYS_ARMING,// йде відлік затримки постановки (потенціометр задає тривалість)
    SYS_ARMED,// на охороні: спрацювання будь-якої зони = тривога
    SYS_ALARM,// тривога активна: сирена, доки не роззброять (BTN1 коротко)
} system_state_t;

// Один запис на кожну "зону" — теж POD, статичний масив, без динамічної пам'яті
typedef struct
{
    const char *name;
    bool bypassed;// виключена з контролю (довгий натиск відповідної кнопки)
    bool tripped;// спрацювала (для логів/діагностики)
} zone_t;

#define N_ZONES 2
#define BTN_ARM_PIN GPIO_NUM_4
#define ARM_DELAY_MIN_S 3u
#define ARM_DELAY_MAX_S 15u

static zone_t s_zones[N_ZONES] =
{
    { .name = "ZONE1", .bypassed = false, .tripped = false },
    { .name = "ZONE2", .bypassed = false, .tripped = false }
};

// volatile: пишеться з крос-контексту, читається з main-циклу
static volatile bool s_panic_latched = false;
static system_state_t s_sys_state = SYS_DISARMED;
static uint32_t s_state_enter_ms= 0;
static uint32_t s_arming_total_s= 0;// скільки секунд триватиме ARMING (з потенціометра)
static bool s_duress = false;

#define N_FSM_BUTTONS 3
enum
{
    IDX_ARM = 0,
    IDX_ZONE1,
    IDX_ZONE2
};
static button_fsm_t s_buttons[N_FSM_BUTTONS];

static QueueHandle_t s_chirp_queue;

static void request_chirp(uint32_t ms)
{
    xQueueSend(s_chirp_queue, &ms, 0); // неблокуюче: викликається з esp_timer callback
}

static void buzzer_task(void *arg)
{
    uint32_t ms;
    while (1)
    {
        if (xQueueReceive(s_chirp_queue, &ms, portMAX_DELAY))
        {
            load_buzzer_chirp(ms);// блокуючий vTaskDelay — тут це безпечно, це власна задача
        }
    }
}

static void sys_transition(system_state_t new_state, uint32_t now_ms)
{
    ESP_LOGW(TAG, "СИСТЕМА: %d -> %d", s_sys_state, new_state);
    s_sys_state = new_state;
    s_state_enter_ms = now_ms;
}

static uint32_t arming_delay_seconds(void)
{
    uint16_t raw = pot_sensor_get_raw(); // 0..4095
    return ARM_DELAY_MIN_S + ((uint32_t)raw * (ARM_DELAY_MAX_S - ARM_DELAY_MIN_S)) / 4095u;
}

static void handle_arm_button(button_event_t ev, uint32_t now_ms)
{
    if (ev == BTN_EVENT_SHORT_PRESS)
    {
        switch (s_sys_state)
        {
            case SYS_DISARMED:
                s_arming_total_s = arming_delay_seconds();
                sys_transition(SYS_ARMING, now_ms);
                request_chirp(60);
                ESP_LOGI(TAG, "Постановка на охорону: %lu с до ARMED", (unsigned long)s_arming_total_s);
                break;

            case SYS_ARMING:
                sys_transition(SYS_DISARMED, now_ms);
                // скасували постановку
                request_chirp(200);
                break;

            case SYS_ARMED:
                sys_transition(SYS_DISARMED, now_ms);
                request_chirp(300);
                ESP_LOGI(TAG, "Знято з охорони.");
                break;

            case SYS_ALARM:
                sys_transition(SYS_DISARMED, now_ms);
                // коротке натискання = роззброїти й скинути тривогу
                load_relay_set(false);
                load_buzzer_set(false);
                s_panic_latched = false;
                ESP_LOGW(TAG, "Тривогу скинуто, система роззброєна.");
                break;
        }
    }
    else if (ev == BTN_EVENT_LONG_PRESS)
    {
        // Довге утримання ARM у будь-якому стані = аварійний повний ресет
        for (int i = 0; i < N_ZONES; i++)
        { 
            s_zones[i].bypassed = false;
            s_zones[i].tripped = false;
        }
        s_panic_latched = false;
        sys_transition(SYS_DISARMED, now_ms);
        load_relay_set(false); load_buzzer_set(false);
        ESP_LOGW(TAG, "ПОВНИЙ РЕСЕТ ПАНЕЛІ (довге утримання ARM).");
        s_duress = true;
    }
}

static void handle_zone_button(int zone_idx, button_event_t ev)
{
    zone_t *z = &s_zones[zone_idx];
    if (ev == BTN_EVENT_SHORT_PRESS)
    {
        z->tripped = true;
        ESP_LOGI(TAG, "%s: симуляція спрацювання датчика (bypass=%d)", z->name, z->bypassed);
        if (s_sys_state == SYS_ARMED && !z->bypassed)
        {
            sys_transition(SYS_ALARM, (uint32_t)(esp_timer_get_time() / 1000ULL));
            ESP_LOGE(TAG, "!!! ТРИВОГА: %s !!!", z->name);
        }
    }
    else if (ev == BTN_EVENT_LONG_PRESS)
    {
        z->bypassed = !z->bypassed;
        request_chirp(z->bypassed ? 150 : 50);
        ESP_LOGW(TAG, "%s: bypass %s", z->name, z->bypassed ? "УВІМКНЕНО" : "ВИМКНЕНО");
    }
}

// Викликається кожні 10 мс з esp_timer
void app_fsm_tick_all(uint32_t now_ms)
{
    button_event_t ev;
    ev = button_fsm_tick(&s_buttons[IDX_ARM], now_ms);
    if (ev != BTN_EVENT_NONE)
    {
        handle_arm_button(ev, now_ms);
    }

    ev = button_fsm_tick(&s_buttons[IDX_ZONE1], now_ms);

    if (ev != BTN_EVENT_NONE)
    {
        handle_zone_button(0, ev);
    }

    ev = button_fsm_tick(&s_buttons[IDX_ZONE2], now_ms);
    
    if (ev != BTN_EVENT_NONE)
    {
        handle_zone_button(1, ev);
    }
    
    // --- Системний перехід ARMING -> ARMED за часом (не подія кнопки, а таймаут) ---

    if (s_sys_state == SYS_ARMING)
    {
        uint32_t elapsed_s = (now_ms - s_state_enter_ms) / 1000u;
        if (elapsed_s >= s_arming_total_s)
        {
            sys_transition(SYS_ARMED, now_ms);
            request_chirp(500);
            ESP_LOGW(TAG, "СИСТЕМА ОЗБРОЄНА.");
        }
    }

    // --- PANIC має найвищий пріоритет і "перебиває" систему в БУДЬ-ЯКОМУ стані ---
    if (s_panic_latched && s_sys_state != SYS_ALARM)
    {
        sys_transition(SYS_ALARM, now_ms);
    }

    // --- Виконавчі дії стану ALARM тримаємо активними, доки не скинуть (handle_arm_button) ---
    if (s_sys_state == SYS_ALARM)
    {
        load_relay_set(true);
        load_buzzer_set(true);
    }
}

// Реакція на PANIC-переривання — виконується у panic_task, НЕ в ISR
static void on_panic_deferred(void)
{
    s_panic_latched = true;
    // сама зміна стану FSM відбудеться в app_fsm_tick_all (наступний тік, ≤10мс)
    ESP_LOGE(TAG, "PANIC зафіксовано, тривога буде піднята на найближчому тіку FSM.");
}

static void arming_beep_task(void *arg)
{
    uint32_t duress_alarm_interval_ms = 3000ULL;
    uint32_t duress_last_time = 0;
    bool duress_alarm_state = false;

    uint32_t armed_beep_interval_ms = 5000ULL;
    uint32_t armed_beep_last_time = 0;

    while (1)
    {
        if (s_sys_state == SYS_ARMING)
        {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint32_t elapsed_s = (now_ms - s_state_enter_ms) / 1000u;
            uint32_t remaining_s = (elapsed_s < s_arming_total_s) ? (s_arming_total_s - elapsed_s) : 0;

            // чим менше залишилось часу, тим частіший писк: 500 мс (спокійно) .. 80 мс (терміново)
            uint32_t beep_period_ms = 80 + (remaining_s * 420) / (ARM_DELAY_MAX_S);
            load_buzzer_set(true);
            vTaskDelay(pdMS_TO_TICKS(30));

            // короткий "тік", а не довгий сигнал
            load_buzzer_set(false);

            if (s_duress)
            {
                load_relay_set(false);
            }

            vTaskDelay(pdMS_TO_TICKS(beep_period_ms));
        }
        else if (s_sys_state == SYS_DISARMED && s_duress)
        {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (now_ms - duress_last_time >= duress_alarm_interval_ms)
            {
                duress_last_time = now_ms;
                duress_alarm_state = !duress_alarm_state;
                load_relay_set(duress_alarm_state);
            }

            vTaskDelay(pdMS_TO_TICKS(100));

        }
        else if (s_sys_state == SYS_ARMED)
        {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (now_ms - armed_beep_last_time >= armed_beep_interval_ms)
            {
                armed_beep_last_time = now_ms;
                load_buzzer_chirp(100);
            }
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100));// не ARMING — просто чекаємо, нічого не робимо
        }
    }
}

void app_main()
{
    load_driver_init();
    button_fsm_init(&s_buttons[IDX_ARM], GPIO_NUM_4);
    button_fsm_init(&s_buttons[IDX_ZONE1], GPIO_NUM_5);
    button_fsm_init(&s_buttons[IDX_ZONE2], GPIO_NUM_6);

    panic_sensor_init(on_panic_deferred);
    pot_sensor_init();

    s_chirp_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(buzzer_task, "buzzer_task", 3072, NULL, 4, NULL);
    button_fsm_start_timer();

    xTaskCreate(arming_beep_task, "arming_beep", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "Пульт охорони периметра запущено. Стан: DISARMED.");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "стан=%d zone1(byp=%d,trip=%d) zone2(byp=%d,trip=%d) pot=%u",s_sys_state,
        s_zones[0].bypassed, s_zones[0].tripped,
        s_zones[1].bypassed, s_zones[1].tripped,
        pot_sensor_get_raw());
    }
}