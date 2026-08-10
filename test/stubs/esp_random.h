#pragma once
#include <stdint.h>
uint32_t esp_random();
void esp_fill_random(void*, size_t);
