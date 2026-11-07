#pragma once
#include "driver/gpio.h"

#define LOAD_BUZZER_PIN GPIO_NUM_11
#define LOAD_RELAY_PIN GPIO_NUM_12

void load_driver_init(void);
void load_relay_set(bool on);
void load_buzzer_set(bool on);
void load_buzzer_chirp(uint32_t ms);
void load_buzzer_set_volume(uint8_t percent);