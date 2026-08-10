// Заглушка ядра задач для проверки сборки на компьютере.
//
// На плате эти типы приходят из FreeRTOS через Arduino.h. Здесь объявляем их явно и в
// том же месте, что и настоящая библиотека, — чтобы файл, который их использует,
// проверялся так же, как собирается.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef unsigned int StackType_t;
typedef struct { int dummy; } StaticTask_t;
typedef void* TaskHandle_t;

#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xFFFFFFFF
