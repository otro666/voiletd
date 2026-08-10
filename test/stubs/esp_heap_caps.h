#pragma once
#include <stdint.h>
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_DMA 8
void* heap_caps_malloc(size_t, uint32_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_total_size(uint32_t);
void heap_caps_malloc_extmem_enable(size_t);
