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
  };

  ButtonNavigator buttonNavigator;
  std::vector<Entry> entries;
  size_t selectorIndex = 0;
  bool configured = false;
  // True once a long-press of Confirm has fired the sync, so the subsequent
  // release doesn't also trigger the short-press Open action.
  bool syncTriggeredByLongPress = false;

  void loadEntries();
};
