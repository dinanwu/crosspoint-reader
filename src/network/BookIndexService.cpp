#include "BookIndexService.h"

#include <Arduino.h>
#include <Epub.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include "activities/Activity.h"
#include "activities/ActivityManager.h"

namespace {
// Matches the ActivityManagerRender task where this work previously ran
// synchronously. 4096 overflowed during the content.opf expat pass (SP went
// ~136 bytes below the floor); expat's recursive handlers plus our parser
// frames need the headroom.
constexpr uint32_t INDEX_TASK_STACK_BYTES = 8192;
constexpr UBaseType_t INDEX_TASK_PRIORITY = 1;
}  // namespace

BookIndexService& BookIndexService::instance() {
  static BookIndexService svc;
  return svc;
}

BookIndexService::BookIndexService() : mutex_(xSemaphoreCreateMutex()) {}

bool BookIndexService::isRunning() const {
  bool r = false;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    r = running_;
    xSemaphoreGive(mutex_);
  }
  return r;
}

bool BookIndexService::abortRequested() const {
  bool r = false;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    r = abortRequested_;
    xSemaphoreGive(mutex_);
  }
  return r;
}

bool BookIndexService::start(Epub* epub) {
  if (!epub) return false;

  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    if (running_) {
      xSemaphoreGive(mutex_);
      LOG_DBG("IDX", "Index build already running, start request ignored");
      return false;
    }
    epub_ = epub;
    running_ = true;
    abortRequested_ = false;
    xSemaphoreGive(mutex_);
  }

  const BaseType_t rc =
      xTaskCreate(&taskTrampoline, "BookIndex", INDEX_TASK_STACK_BYTES, this, INDEX_TASK_PRIORITY, nullptr);
  if (rc != pdPASS) {
    LOG_ERR("IDX", "Failed to create index task");
    if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
      epub_ = nullptr;
      running_ = false;
      xSemaphoreGive(mutex_);
    }
    return false;
  }
  return true;
}

void BookIndexService::cancel() {
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    abortRequested_ = true;
    xSemaphoreGive(mutex_);
  }
}

void BookIndexService::cancelAndWait(const Epub* epub) {
  // Only wait if the task is running for THIS epub — otherwise we could block
  // the destructor of an Epub that was never registered.
  bool shouldWait = false;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    if (running_ && epub_ == epub) {
      abortRequested_ = true;
      shouldWait = true;
    }
    xSemaphoreGive(mutex_);
  }
  if (!shouldWait) return;

  // Busy-wait at a coarse interval; the task observes abortRequested_ at phase
  // boundaries. Worst case ≈ one ZIP central-directory scan (~1-2s).
  while (isRunning()) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void BookIndexService::taskTrampoline(void* ctx) { static_cast<BookIndexService*>(ctx)->taskBody(); }

void BookIndexService::taskBody() {
  // Keep the CPU at full speed for the duration of the build. Without this the
  // 3-second idle-timer in main.cpp would drop us to 10MHz and stall the
  // indexer 16x while the user is reading.
  HalPowerManager::Lock cpuLock;

  Epub* epub = nullptr;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    epub = epub_;
    xSemaphoreGive(mutex_);
  }

  bool success = false;
  if (epub) {
    success = epub->runBackgroundCacheBuild(
        [](void* ctx) { return static_cast<BookIndexService*>(ctx)->abortRequested(); }, this);
  } else {
    LOG_ERR("IDX", "Task started without a valid Epub pointer");
  }

  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    epub_ = nullptr;
    running_ = false;
    xSemaphoreGive(mutex_);
  }
  // Wake the render task so the IDX indicator disappears and any newly-visible
  // status-bar fields (progress %, chapter title) get drawn immediately.
  activityManager.requestUpdate(true);

  LOG_DBG("IDX", "Index task exiting (success=%d), stack high water: %u", success,
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

  vTaskDelete(nullptr);
}
