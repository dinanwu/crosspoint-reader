#pragma once

#include "activities/Activity.h"
#include "network/SubscriptionSyncer.h"

class SubscriptionSyncActivity final : public Activity {
 public:
  explicit SubscriptionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SubscriptionSync", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool skipLoopDelay() override { return true; }

 private:
  SubscriptionSyncer syncer;
  bool running = false;
  unsigned long terminalEnteredAt = 0;
  SubscriptionSyncer::Phase lastRenderedPhase = SubscriptionSyncer::Phase::Idle;
  int lastRenderedPercent = -1;
  std::string lastRenderedTitle;
};
