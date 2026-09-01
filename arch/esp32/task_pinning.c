/*
 * arch/esp32/task_pinning.c
 *
 * Pins all FreeRTOS tasks with no explicit core affinity (tskNO_AFFINITY) to
 * core 0, keeping core 1 exclusively for the Arduino loop (loopTask) where
 * LoRa packet processing runs.
 *
 * Background:
 *   On ESP32-S3 (dual-core), ARDUINO_RUNNING_CORE=1 pins loopTask to core 1.
 *   However, ESP-IDF v4 creates MQTT task with tskNO_AFFINITY, so FreeRTOS
 *   may schedule them on core 1 under load, preempting LoRa processing.
 *
 *   ESP-IDF v4 provides no public API to change a task's core affinity after
 *   creation (vTaskCoreAffinitySet is IDF v5+ only), and esp_mqtt_client_config_t
 *   has no core selection field in IDF v4.  The linker --wrap mechanism is the
 *   only way to intercept xTaskCreatePinnedToCore at the call site inside
 *   precompiled library code.
 *
 *   It's intentionally do not filtered by task name — this keeps the approach
 *   robust against internal ESP-IDF task name changes and also catches any
 *   other library tasks that may happen to be created with tskNO_AFFINITY.
 *   Core 1 is reserved exclusively for loopTask (LoRa / application logic).
 *
 * Mechanism:
 *   -Wl,--wrap=xTaskCreatePinnedToCore redirects every call to
 *   xTaskCreatePinnedToCore (including the xTaskCreate macro which expands to
 *   it with tskNO_AFFINITY) through __wrap_xTaskCreatePinnedToCore below.
 *   Tasks that are already explicitly pinned to a core are passed through
 *   unchanged.
 *
 * Verified task layout after this patch:
 *   core 0: mqtt_task(5), httpd(2), tiT/LwIP(18), wifi(23), esp_timer(22), ipc0(24)
 *   core 1: loopTask(1), ipc1(24), IDLE1(0)
 *
 * Note: this file is compiled only for [esp32_base] targets (IDF v4).
 * ESP32-C6 uses pioarduino (IDF v5) where vTaskCoreAffinitySet() is available.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The real function, renamed by the linker to __real_xTaskCreatePinnedToCore. */
extern BaseType_t __real_xTaskCreatePinnedToCore(
  TaskFunction_t pvTaskCode,
  const char * const pcName,
  const uint32_t usStackDepth,
  void * const pvParameters,
  UBaseType_t uxPriority,
  TaskHandle_t * const pvCreatedTask,
  const BaseType_t xCoreID
);

BaseType_t __wrap_xTaskCreatePinnedToCore(
  TaskFunction_t pvTaskCode,
  const char * const pcName,
  const uint32_t usStackDepth,
  void * const pvParameters,
  UBaseType_t uxPriority,
  TaskHandle_t * const pvCreatedTask,
  const BaseType_t xCoreID
) {
  /* Pin all tasks with no affinity to core 0. */
  BaseType_t core = (xCoreID == tskNO_AFFINITY) ? 0 : xCoreID;
  return __real_xTaskCreatePinnedToCore(
    pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pvCreatedTask, core
  );
}
