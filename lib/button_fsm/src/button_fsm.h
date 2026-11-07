#include <soc/gpio_num.h>

#define PANIC_PIN GPIO_NUM_7

typedef enum
{
    BTN_STATE_IDLE = 0,
    BTN_STATE_PRESSED,
    BTN_STATE_HELD,
    BTN_STATE_RELEASE
} button_fsm_state;

typedef enum
{
    BTN_EVENT_NONE = 0,
    BTN_EVENT_SHORT_PRESS,
    BTN_EVENT_LONG_PRESS
} button_event_t;

typedef struct
{
    gpio_num_t pin;
    button_fsm_state state;
    uint32_t state_enter_ms;
    bool long_fired;
} button_fsm_t;


void panic_sensor_init(void (*on_panic_deferred)(void));
void button_fsm_start_timer(void);
void app_fsm_tick_all(uint32_t now_ms);
button_event_t button_fsm_tick(button_fsm_t *btn, uint32_t now_ms);
void button_fsm_init(button_fsm_t *btn, gpio_num_t pin);