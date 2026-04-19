#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

#include "SubscriptionSyncer.h"

// Wraps SubscriptionSyncer in a FreeRTOS task so the UI thread never blocks on
// Wi-Fi / HTTP. Any activity can read progress via fullSnapshot(). A single
// global instance.
class SubscriptionSyncService {
 public:
  static SubscriptionSyncService& instance();

  // Returns true if a task was spawned. Returns false if sync is already
  // running, or preconditions (enabled, URL, token, Wi-Fi creds) are not met.
  bool startIfIdle();

  // Request an orderly stop; the task drains and exits within ~200ms during
  // Wi-Fi connect or downloads, or up to ~3s during NTP.
  void cancel();

  bool isRunning() const { return running_.load(std::memory_order_acquire); }

  // Lock-free accessor for UI polling. Monotonic per-run — advances once when
  // each sync task exits. Zero until the first sync has ever finished.
  uint64_t lastFinishedAtMs() const { return lastFinishedAtMs_.load(std::memory_order_acquire); }

  struct LastResult {
    SubscriptionSyncer::Phase phase = SubscriptionSyncer::Phase::Idle;
    SubscriptionSyncer::FailureReason failure = SubscriptionSyncer::FailureReason::None;
    bool anyChanges = false;
    uint64_t finishedAtMs = 0;
  };

  // One mutex take returns a consistent view — avoids a render-time race where
  // separate progress/lastResult/running reads see a half-landed transition.
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
  static void onSyncerProgress(void* ctx);
  void publishProgress();

  SubscriptionSyncer syncer_;
  SubscriptionSyncer::Progress progress_;  // guarded by mutex_
  LastResult lastResult_;                  // guarded by mutex_
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> lastFinishedAtMs_{0};
  // Written only from the SubSync task (progress callback), read outside mutex.
  uint32_t lastPublishMs_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
};
