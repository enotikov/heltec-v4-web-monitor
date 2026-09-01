#pragma once
#ifdef ESP32

#include <stdint.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class CPUUsageTracker {
public:
  void begin();

  float getCore0Util() const {
    return (float)_sma_sum * (1.0f / (SMA_WINDOW * 255.0f));
  }

private:
  static constexpr uint8_t SMA_WINDOW = 64;  // power of 2 — enables & mask

  static volatile uint32_t s_ticks[2];  // [0] = idle ticks, [1] = busy ticks
  static TaskHandle_t      s_idle_handle;

  static void IRAM_ATTR s_tick_hook();
  static void           s_sample_cb(void* arg);

  uint32_t _last_idle = 0;
  uint32_t _last_busy = 0;

  uint8_t  _sma_buf[SMA_WINDOW] = {};
  uint8_t  _sma_idx = 0;

  volatile uint16_t _sma_sum = 0;

  esp_timer_handle_t _timer = nullptr;

  void _onSample();
};

#endif
