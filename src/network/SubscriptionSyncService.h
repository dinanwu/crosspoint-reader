#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>

#include "SubscriptionSyncer.h"

// Owns a FreeRTOS task that runs the subscription sync end-to-end so the UI
// thread never has to block on Wi-Fi / HTTP. A single global instance is the
// only writer to the task handle; any activity can read progress/status via
// snapshot() and lastResult() under the internal mutex.
//
// Lifecycle:
//   - startIfIdle() spawns the task if preconditions pass and none is running.
//   - The task runs syncer.tick() until terminal, publishes progress on each
//     update, records the result, then vTaskDelete(nullptr) clears itself.
//   - cancel() sets the syncer's abort flag; the task observes it at the next
//     phase boundary or during the HTTP download's progress callback.
class SubscriptionSyncService {
 public:
  static SubscriptionSyncService& instance();

  // Returns true if a task was spawned. Returns false if sync is already
  // running, or preconditions (enabled, URL, token, Wi-Fi creds) are not met.
  bool startIfIdle();

  // Request an orderly stop; the task drains and exits within ~200ms during
  // Wi-Fi connect or downloads, or up to ~3s during NTP.
  void cancel();

  bool isRunning() const;

  // Copy of the live progress struct. Safe to call from any task.
  SubscriptionSyncer::Progress snapshot() const;

  struct LastResult {
    SubscriptionSyncer::Phase phase = SubscriptionSyncer::Phase::Idle;
    SubscriptionSyncer::FailureReason failure = SubscriptionSyncer::FailureReason::None;
    bool anyChanges = false;
    uint64_t finishedAtMs = 0;
  };
  LastResult lastResult() const;

  // Combined snapshot for UI render paths that would otherwise take the mutex
  // three times per frame (running + snapshot + lastResult). A single take also
  // gives a consistent view — isRunning() and snapshot() can't disagree about
  // whether the last state transition has landed.
  struct StateSnapshot {
    SubscriptionSyncer::Progress progress;
    LastResult lastResult;
    bool running = false;
  };
  StateSnapshot fullSnapshot() const;

 private:
  SubscriptionSyncService();
  SubscriptionSyncService(const SubscriptionSyncService&) = delete;
  SubscriptionSyncService& operator=(const SubscriptionSyncService&) = delete;

  static void taskTrampoline(void* ctx);
  void taskBody();

  // Copies syncer_.progress() into progress_ under mutex_ and pokes the render
  // task. Called both at phase boundaries (by taskBody) and during long HTTP
  // downloads (by the progress listener set on syncer_).
  void publishProgress();

  SubscriptionSyncer syncer_;
  SubscriptionSyncer::Progress progress_;  // guarded by mutex_
  LastResult lastResult_;                  // guarded by mutex_
  bool running_ = false;                   // guarded by mutex_
  uint32_t lastPublishMs_ = 0;             // guarded by mutex_
  SemaphoreHandle_t mutex_ = nullptr;
};
