#pragma once
#include <stdint.h>

void pot_sensor_init(void);// старт періодичного esp_timer на 50 мс
uint16_t pot_sensor_get_raw(void);// останнє прочитане значення 0..4095 (кеш, без блокувань)