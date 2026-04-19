#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

// List of subscribed EPUBs. Unread count per row = lastKnownSpineCount minus
// the watermark sidecar value. Opening a row goes to EpubReaderActivity via
// the normal path and the reader resumes at saved progress.
class SubscriptionsInboxActivity final : public Activity {
 public:
  explicit SubscriptionsInboxActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SubscriptionsInbox", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Entry {
    std::string title;
    std::string localPath;
    uint16_t unreadCount = 0;
    uint64_t lastSyncedMs = 0;
  };

  ButtonNavigator buttonNavigator;
  std::vector<Entry> entries;
  size_t selectorIndex = 0;
  bool configured = false;
  // Latched on long-press Confirm so the matching release doesn't also fire short-press Open.
  bool syncTriggeredByLongPress = false;
  // Primed in onEnter; loop() reloads entries when a new sync finishes while visible.
  uint64_t lastSeenResultMs = 0;

  void loadEntries();
  void triggerSyncOrWifi();
  void launchWifiSelection(bool startSyncOnSuccess);
  void onWifiSelectionComplete(bool connected, bool startSyncOnSuccess);
};