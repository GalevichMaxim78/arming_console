#include "pot_sensor.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"

#define POT_ADC_UNIT ADC_UNIT_1
#define POT_ADC_CHANNEL ADC_CHANNEL_9 // GPIO10 на ESP32-S3 = ADC1_CH9
#define POT_SAMPLE_MS 50u

static adc_oneshot_unit_handle_t s_adc_handle;
static volatile uint16_t s_last_raw = 0; // volatile: пише таймер, читає main/pot_sensor_get_raw

static void pot_timer_cb(void *arg)
{
    int raw = 0;
    adc_oneshot_read(s_adc_handle, POT_ADC_CHANNEL, &raw);
    s_last_raw = (uint16_t)raw;
}

void pot_sensor_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg =
    {
         .unit_id = POT_ADC_UNIT
    };

    adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    adc_oneshot_chan_cfg_t chan_cfg =
    {
        .bitwidth = ADC_BITWIDTH_DEFAULT,// 12 біт на ESP32-S3 → 0..4095
        .atten = ADC_ATTEN_DB_12,// дозволяє міряти до ~3.3В (замість дефолтних ~1.1В)
    };

    adc_oneshot_config_channel(s_adc_handle, POT_ADC_CHANNEL, &chan_cfg);
    const esp_timer_create_args_t args =
    {
        .callback = &pot_timer_cb,
        .name = "pot_sample",
    };

    esp_timer_handle_t timer;
    esp_timer_create(&args, &timer);
    esp_timer_start_periodic(timer, POT_SAMPLE_MS * 1000ULL);// esp_timer рахує в МІКРОсекундах!
}

uint16_t pot_sensor_get_raw(void)
{
    return s_last_raw; // volatile uint16_t читається атомарно на 32-бітному ядрі — гонки даних немає
}