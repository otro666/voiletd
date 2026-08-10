#pragma once
#include <stdint.h>
#define ESP_INTR_FLAG_LEVEL1 2
typedef int i2s_port_t;
#define I2S_NUM_0 0
#define I2S_NUM_1 1
#define I2S_PIN_NO_CHANGE -1
typedef enum { I2S_MODE_MASTER=1, I2S_MODE_TX=2, I2S_MODE_RX=4 } i2s_mode_t;
typedef enum { I2S_BITS_PER_SAMPLE_16BIT=16 } i2s_bits_per_sample_t;
typedef enum { I2S_CHANNEL_FMT_RIGHT_LEFT=0, I2S_CHANNEL_FMT_ONLY_LEFT=1 } i2s_channel_fmt_t;
typedef enum { I2S_COMM_FORMAT_STAND_I2S=1 } i2s_comm_format_t;
typedef struct { i2s_mode_t mode; int sample_rate; i2s_bits_per_sample_t bits_per_sample; i2s_channel_fmt_t channel_format; i2s_comm_format_t communication_format; int intr_alloc_flags; int dma_buf_count; int dma_buf_len; bool use_apll; int fixed_mclk; } i2s_config_t;
typedef struct { int mck_io_num, bck_io_num, ws_io_num, data_out_num, data_in_num; } i2s_pin_config_t;
int i2s_driver_install(i2s_port_t, const i2s_config_t*, int, void*);
int i2s_set_pin(i2s_port_t, const i2s_pin_config_t*);
int i2s_start(i2s_port_t);
int i2s_stop(i2s_port_t);
int i2s_zero_dma_buffer(i2s_port_t);
int i2s_write(i2s_port_t, const void*, size_t, size_t*, uint32_t);
int i2s_read(i2s_port_t, void*, size_t, size_t*, uint32_t);
#define portMAX_DELAY 0xFFFFFFFF
