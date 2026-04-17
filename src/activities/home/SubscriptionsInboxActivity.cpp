#include "SubscriptionsInboxActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/network/SubscriptionSyncActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/SubscriptionState.h"

void SubscriptionsInboxActivity::loadEntries() {
  entries.clear();

  SubscriptionState state;
  if (!state.load()) {
    return;
  }

  const auto ids = state.unreadSeriesIds();
  entries.reserve(ids.size());
  for (const auto& id : ids) {
    auto it = state.seriesMeta.find(id);
    if (it == state.seriesMeta.end()) continue;
    const auto& meta = it->second;
    const uint16_t watermark = SubscriptionState::readWatermark(meta.localPath);
    if (meta.lastKnownSpineCount <= watermark) continue;

    Entry e;
    e.title = meta.title.empty() ? id : meta.title;
    e.localPath = meta.localPath;
    e.unreadCount = meta.lastKnownSpineCount - watermark;
    entries.push_back(std::move(e));
  }
}

void SubscriptionsInboxActivity::onEnter() {
  Activity::onEnter();
  configured = SETTINGS.subscriptionsEnabled && strlen(SETTINGS.subscriptionServerUrl) > 0 &&
               strlen(SETTINGS.subscriptionBearerToken) > 0;
  if (configured) {
    loadEntries();
  }
  selectorIndex = 0;
  requestUpdate();
}

void SubscriptionsInboxActivity::onExit() {
  Activity::onExit();
  entries.clear();
}

void SubscriptionsInboxActivity::loop() {
  const int listSize = static_cast<int>(entries.size());
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  using Button = MappedInputManager::Button;

  if (mappedInput.wasReleased(Button::Back)) {
    onGoHome();
    return;
  }

  if (!configured) {
    if (mappedInput.wasReleased(Button::Confirm)) {
      // Launch the settings web server so the user can configure subscriptions
      // from a phone or laptop. This is the same activity as File Transfer.
      activityManager.goToFileTransfer();
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Confirm)) {
    if (!entries.empty() && selectorIndex < entries.size()) {
      onSelectBook(entries[selectorIndex].localPath);
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Left)) {
    // Manual sync. On return, refresh the inbox to pick up any new entries.
    startActivityForResult(std::make_unique<SubscriptionSyncActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) {
                             loadEntries();
                             if (selectorIndex >= entries.size()) {
                               selectorIndex = entries.empty() ? 0 : entries.size() - 1;
                             }
                           });
    return;
  }

  // Navigation: side Up = previous, side Down or front Right = next.
  // Left is reserved for Sync, so we use explicit button lists rather than
  // ButtonNavigator::onPreviousRelease (which also listens to Left).
  buttonNavigator.onRelease({Button::Up}, [this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onContinuous({Button::Up}, [this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void SubscriptionsInboxActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SUBSCRIPTIONS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (!configured) {
    const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int y = contentTop + contentHeight / 2 - lineHeight * 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SUBS_NOT_CONFIGURED), true, EpdFontFamily::BOLD);
    y += lineHeight * 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SUBS_SETUP_HINT_1));
    y += lineHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SUBS_SETUP_HINT_2));
    y += lineHeight;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SUBS_SETUP_HINT_3));

    const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_SETUP), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (entries.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_SYNC_NO_CHANGES));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, entries.size(), selectorIndex,
        [this](int index) { return entries[index].title; },
        [this](int index) {
          char buf[32];
          snprintf(buf, sizeof(buf), tr(STR_INBOX_UNREAD_COUNT), entries[index].unreadCount);
          return std::string(buf);
        },
        [this](int index) { return UITheme::getFileIcon(entries[index].localPath); });
  }

  const char* confirmLabel = entries.empty() ? "" : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirmLabel, tr(STR_SYNC_NOW), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
