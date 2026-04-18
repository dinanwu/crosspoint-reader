#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

// List of subscribed EPUBs with unread new chapters. Populated from
// SubscriptionState + per-book watermark sidecars. Opening a row goes to
// EpubReaderActivity via the normal path — the reader resumes at saved
// progress and surfaces the break page when the user walks into new chapters.
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
  // True once a long-press of Confirm has fired the sync, so the subsequent
  // release doesn't also trigger the short-press Open action.
  bool syncTriggeredByLongPress = false;
  // Tracks the most recent SubscriptionSyncService result we've reacted to, so
  // loadEntries() runs exactly once per completed background sync while the
  // Inbox is visible.
  uint64_t lastSeenResultMs = 0;

  void loadEntries();
  // Routes Confirm to either sync or Wi-Fi setup based on whether credentials
  // are saved — saves the user one round-trip through a NoCredentials failure
  // banner when they haven't connected yet.
  void triggerSyncOrWifi();
  void launchWifiSelection(bool startSyncOnSuccess);
  void onWifiSelectionComplete(bool connected, bool startSyncOnSuccess);
};