#pragma once
#include "FreeRTOS.h"

void vTaskDelay(uint32_t);
int xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, int,
                            TaskHandle_t*, int);
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*)(void*), const char*, uint32_t, void*,
                                           int, StackType_t*, StaticTask_t*, int);
