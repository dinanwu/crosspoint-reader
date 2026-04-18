#include "SubscriptionSyncService.h"

#include <Arduino.h>
#include <Logging.h>

#include "activities/Activity.h"
#include "activities/ActivityManager.h"

namespace {
// HTTP + TLS + JSON live on this stack during sync. Matches the sizing used
// by other network-heavy paths.
constexpr uint32_t SYNC_TASK_STACK_BYTES = 4096;
constexpr UBaseType_t SYNC_TASK_PRIORITY = 1;
}  // namespace

SubscriptionSyncService& SubscriptionSyncService::instance() {
  static SubscriptionSyncService svc;
  return svc;
}

SubscriptionSyncService::SubscriptionSyncService() : mutex_(xSemaphoreCreateMutex()) {}

bool SubscriptionSyncService::isRunning() const {
  bool r = false;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    r = running_;
    xSemaphoreGive(mutex_);
  }
  return r;
}

bool SubscriptionSyncService::startIfIdle() {
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    if (running_) {
      xSemaphoreGive(mutex_);
      LOG_DBG("SUB", "Sync already running, start request ignored");
      return false;
    }
    running_ = true;
    xSemaphoreGive(mutex_);
  }

  // syncer.begin() validates preconditions (enabled/URL/token/Wi-Fi creds).
  // On failure, unwind running_ so the next caller can retry.
  if (!syncer_.begin()) {
    if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
      running_ = false;
      xSemaphoreGive(mutex_);
    }
    return false;
  }

  syncer_.setProgressListener([this] { publishProgress(); });

  // Publish initial snapshot so a reader that polls right after startIfIdle()
  // sees a non-Idle phase.
  publishProgress();

  const BaseType_t rc =
      xTaskCreate(&taskTrampoline, "SubSync", SYNC_TASK_STACK_BYTES, this, SYNC_TASK_PRIORITY, nullptr);
  if (rc != pdPASS) {
    LOG_ERR("SUB", "Failed to create sync task");
    if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
      running_ = false;
      xSemaphoreGive(mutex_);
    }
    return false;
  }
  return true;
}

void SubscriptionSyncService::cancel() {
  if (!isRunning()) return;
  syncer_.cancel();
}

SubscriptionSyncer::Progress SubscriptionSyncService::snapshot() const {
  SubscriptionSyncer::Progress copy;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    copy = progress_;
    xSemaphoreGive(mutex_);
  }
  return copy;
}

SubscriptionSyncService::LastResult SubscriptionSyncService::lastResult() const {
  LastResult copy;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    copy = lastResult_;
    xSemaphoreGive(mutex_);
  }
  return copy;
}

SubscriptionSyncService::StateSnapshot SubscriptionSyncService::fullSnapshot() const {
  StateSnapshot copy;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    copy.progress = progress_;
    copy.lastResult = lastResult_;
    copy.running = running_;
    xSemaphoreGive(mutex_);
  }
  return copy;
}

void SubscriptionSyncService::publishProgress() {
  // The HTTP progress callback fires on every ~1500-byte TCP chunk (~1400x for a
  // 2 MB EPUB). Without a throttle, each call wakes the render task and kicks a
  // full e-ink refresh — saturating the display queue and hammering the SD card
  // with icon/cover reads. Phase or title changes always publish immediately so
  // state transitions stay snappy; within-phase byte progress caps at ~4 Hz.
  bool skip = false;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    const auto& live = syncer_.progress();
    const uint32_t now = millis();
    const bool phaseChanged = live.phase != progress_.phase;
    const bool titleChanged = live.currentTitle != progress_.currentTitle;
    if (!phaseChanged && !titleChanged && now - lastPublishMs_ < 250) {
      skip = true;
    } else {
      progress_ = live;
      lastPublishMs_ = now;
    }
    xSemaphoreGive(mutex_);
  }
  if (!skip) {
    // Already safe from non-main tasks (xTaskNotify under the hood).
    activityManager.requestUpdate(true);
  }
}

void SubscriptionSyncService::taskTrampoline(void* ctx) {
  static_cast<SubscriptionSyncService*>(ctx)->taskBody();
}

void SubscriptionSyncService::taskBody() {
  // The initial publishProgress() in startIfIdle() primed the UI with the Idle
  // phase. Only republish from here when the phase actually advances — within-
  // phase byte progress is fed separately by the syncer's progress listener,
  // so publishing after every tick would just queue redundant eink refreshes
  // during the long blocking Wi-Fi/NTP phases.
  auto lastPublishedPhase = syncer_.progress().phase;
  while (!syncer_.isTerminal()) {
    syncer_.tick();
    const auto currentPhase = syncer_.progress().phase;
    if (currentPhase != lastPublishedPhase) {
      publishProgress();
      lastPublishedPhase = currentPhase;
    }
    // Let the render and GPIO tasks run.
    vTaskDelay(1);
  }

  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    const auto& p = syncer_.progress();
    lastResult_.phase = p.phase;
    lastResult_.failure = p.failure;
    lastResult_.anyChanges = p.anyChanges;
    lastResult_.finishedAtMs = static_cast<uint64_t>(millis());
    running_ = false;
    xSemaphoreGive(mutex_);
  }
  activityManager.requestUpdate(true);

  LOG_DBG("SUB", "Sync task exiting, stack high water: %u",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

  vTaskDelete(nullptr);
}
