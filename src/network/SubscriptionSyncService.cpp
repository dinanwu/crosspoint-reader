#include "SubscriptionSyncService.h"

#include <Arduino.h>
#include <Logging.h>

#include "activities/Activity.h"
#include "activities/ActivityManager.h"

namespace {
constexpr uint32_t SYNC_TASK_STACK_BYTES = 4096;
constexpr UBaseType_t SYNC_TASK_PRIORITY = 1;
// Cap within-phase byte-progress publishes to ~4 Hz. Each publish notifies the
// render task, and full e-ink refreshes are measured in seconds — unthrottled
// publishing saturates the display queue during a download.
constexpr uint32_t PROGRESS_PUBLISH_MIN_INTERVAL_MS = 250;
}  // namespace

SubscriptionSyncService& SubscriptionSyncService::instance() {
  static SubscriptionSyncService svc;
  return svc;
}

SubscriptionSyncService::SubscriptionSyncService() : mutex_(xSemaphoreCreateMutex()) {}

bool SubscriptionSyncService::startIfIdle() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    LOG_DBG("SUB", "Sync already running, start request ignored");
    return false;
  }

  if (!syncer_.begin()) {
    running_.store(false, std::memory_order_release);
    return false;
  }

  syncer_.setProgressListener(&SubscriptionSyncService::onSyncerProgress, this);

  // Prime the UI so a poll right after startIfIdle() sees a non-Idle phase.
  publishProgress();

  const BaseType_t rc =
      xTaskCreate(&taskTrampoline, "SubSync", SYNC_TASK_STACK_BYTES, this, SYNC_TASK_PRIORITY, nullptr);
  if (rc != pdPASS) {
    LOG_ERR("SUB", "Failed to create sync task");
    running_.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void SubscriptionSyncService::cancel() {
  if (!isRunning()) return;
  syncer_.cancel();
}

SubscriptionSyncService::StateSnapshot SubscriptionSyncService::fullSnapshot() const {
  StateSnapshot copy;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    copy.progress = progress_;
    copy.lastResult = lastResult_;
    xSemaphoreGive(mutex_);
  }
  copy.running = isRunning();
  return copy;
}

void SubscriptionSyncService::onSyncerProgress(void* ctx) {
  static_cast<SubscriptionSyncService*>(ctx)->publishProgress();
}

void SubscriptionSyncService::publishProgress() {
  // lastPublishMs_ is written only from the SubSync task (progress callback + taskBody).
  // Check the throttle outside the mutex — a stale read can at worst cause one extra
  // publish, never a missed one.
  const auto& live = syncer_.progress();
  const uint32_t now = millis();
  const bool phaseChanged = live.phase != progress_.phase;
  const bool titleChanged = live.currentTitle != progress_.currentTitle;
  if (!phaseChanged && !titleChanged && now - lastPublishMs_ < PROGRESS_PUBLISH_MIN_INTERVAL_MS) {
    return;
  }

  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    progress_ = live;
    xSemaphoreGive(mutex_);
  }
  lastPublishMs_ = now;
  activityManager.requestUpdate(true);
}

void SubscriptionSyncService::taskTrampoline(void* ctx) {
  static_cast<SubscriptionSyncService*>(ctx)->taskBody();
}

void SubscriptionSyncService::taskBody() {
  // Republish only on phase changes — within-phase byte progress comes from the
  // syncer's progress listener. Republishing every tick would queue redundant
  // e-ink refreshes during the long blocking Wi-Fi / NTP phases.
  auto lastPublishedPhase = syncer_.progress().phase;
  while (!syncer_.isTerminal()) {
    syncer_.tick();
    const auto currentPhase = syncer_.progress().phase;
    if (currentPhase != lastPublishedPhase) {
      publishProgress();
      lastPublishedPhase = currentPhase;
    }
    vTaskDelay(1);
  }

  const uint64_t nowMs = static_cast<uint64_t>(millis());
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    const auto& p = syncer_.progress();
    lastResult_.phase = p.phase;
    // Only carry a failure reason on Failed — Done/Cancelled would otherwise
    // latch whatever progress_.failure was at the moment the run ended.
    lastResult_.failure = (p.phase == SubscriptionSyncer::Phase::Failed) ? p.failure : SubscriptionSyncer::FailureReason::None;
    lastResult_.anyChanges = p.anyChanges;
    lastResult_.finishedAtMs = nowMs;
    xSemaphoreGive(mutex_);
  }
  lastFinishedAtMs_.store(nowMs, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  activityManager.requestUpdate(true);

  LOG_DBG("SUB", "Sync task exiting, stack high water: %u",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

  vTaskDelete(nullptr);
}
