#include "SubscriptionsInboxActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/SubscriptionState.h"
#include "network/SubscriptionSyncService.h"

namespace {
const char* phaseLabel(SubscriptionSyncer::Phase p) {
  using Phase = SubscriptionSyncer::Phase;
  switch (p) {
    case Phase::Idle:
    case Phase::ConnectingWifi:
      return tr(STR_SYNC_CONNECTING_WIFI);
    case Phase::SyncingTime:
    case Phase::FetchingIndex:
      return tr(STR_SYNC_FETCHING_INDEX);
    case Phase::DownloadingEpub:
      return tr(STR_SYNC_DOWNLOADING);
    case Phase::Cleaning:
      return tr(STR_SYNC_CLEANING);
    case Phase::Done:
      return tr(STR_SYNC_COMPLETE);
    case Phase::Failed:
      return tr(STR_SYNC_FAILED);
    case Phase::Cancelled:
      return tr(STR_SYNC_CANCELLED);
  }
  return "";
}

const char* failureLabel(SubscriptionSyncer::FailureReason r) {
  using Reason = SubscriptionSyncer::FailureReason;
  switch (r) {
    case Reason::NoCredentials:
      return tr(STR_SYNC_FAIL_NO_CREDS);
    case Reason::WifiConnect:
      return tr(STR_SYNC_FAIL_WIFI);
    case Reason::IndexFetch:
      return tr(STR_SYNC_FAIL_INDEX);
    case Reason::IndexParse:
      return tr(STR_SYNC_FAIL_PARSE);
    case Reason::UnsupportedFormat:
      return tr(STR_SYNC_FAIL_FORMAT);
    case Reason::Unauthorized:
      return tr(STR_SYNC_FAIL_AUTH);
    case Reason::ServerError:
      return tr(STR_SYNC_FAIL_SERVER);
    case Reason::None:
      break;
  }
  return "";
}
}  // namespace

void SubscriptionsInboxActivity::loadEntries() {
  entries.clear();

  SubscriptionState state;
  if (!state.load()) {
    return;
  }

  entries.reserve(state.seriesMeta.size());
  for (const auto& kv : state.seriesMeta) {
    const auto& meta = kv.second;
    if (meta.localPath.empty()) continue;

    const uint16_t watermark = SubscriptionState::readWatermark(meta.localPath);
    const uint16_t unread =
        meta.lastKnownSpineCount > watermark ? static_cast<uint16_t>(meta.lastKnownSpineCount - watermark) : 0;

    Entry e;
    e.title = meta.title.empty() ? kv.first : meta.title;
    e.localPath = meta.localPath;
    e.unreadCount = unread;
    e.lastSyncedMs = meta.lastSyncedMs;
    entries.push_back(std::move(e));
  }

  // Series with new chapters float to the top; within each bucket, newest sync first.
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    const bool aUnread = a.unreadCount > 0;
    const bool bUnread = b.unreadCount > 0;
    if (aUnread != bUnread) return aUnread;
    return a.lastSyncedMs > b.lastSyncedMs;
  });
}

void SubscriptionsInboxActivity::triggerSyncOrWifi() {
  auto& syncService = SubscriptionSyncService::instance();
  if (syncService.isRunning()) {
    syncService.cancel();
    requestUpdate();
    return;
  }
  if (WIFI_STORE.getCredentials().empty()) {
    // Skip the NoCredentials failure-banner round-trip; auto-start sync on success.
    launchWifiSelection(/*startSyncOnSuccess=*/true);
    return;
  }
  syncService.startIfIdle();
  requestUpdate();
}

void SubscriptionsInboxActivity::launchWifiSelection(bool startSyncOnSuccess) {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this, startSyncOnSuccess](const ActivityResult& result) {
                           onWifiSelectionComplete(!result.isCancelled, startSyncOnSuccess);
                         });
}

void SubscriptionsInboxActivity::onWifiSelectionComplete(bool connected, bool startSyncOnSuccess) {
  if (connected && startSyncOnSuccess) {
    SubscriptionSyncService::instance().startIfIdle();
  }
  requestUpdate();
}

void SubscriptionsInboxActivity::onEnter() {
  Activity::onEnter();
  configured = SETTINGS.subscriptionsEnabled && strlen(SETTINGS.subscriptionServerUrl) > 0 &&
               strlen(SETTINGS.subscriptionBearerToken) > 0;
  if (configured) {
    loadEntries();
  }
  // Prime so loop() reloads only when a *new* sync finishes while we're visible.
  lastSeenResultMs = SubscriptionSyncService::instance().lastFinishedAtMs();
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
  constexpr unsigned long LONG_PRESS_SYNC_MS = 1000;

  auto& syncService = SubscriptionSyncService::instance();

  // Pick up newly-downloaded series while the Inbox is still on screen —
  // finishedAtMs is the only monotonic signal for "a sync just sealed".
  const uint64_t finishedAtMs = syncService.lastFinishedAtMs();
  if (finishedAtMs != 0 && finishedAtMs != lastSeenResultMs) {
    lastSeenResultMs = finishedAtMs;
    loadEntries();
    if (selectorIndex >= entries.size()) {
      selectorIndex = entries.empty() ? 0 : (entries.size() - 1);
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(Button::Back)) {
    onGoHome();
    return;
  }

  if (!configured) {
    if (mappedInput.wasReleased(Button::Confirm)) {
      activityManager.goToFileTransfer();
    }
    return;
  }

  if (!syncTriggeredByLongPress && mappedInput.isPressed(Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_SYNC_MS) {
    syncTriggeredByLongPress = true;
    triggerSyncOrWifi();
    return;
  }

  if (mappedInput.wasReleased(Button::Confirm)) {
    if (syncTriggeredByLongPress) {
      syncTriggeredByLongPress = false;
      return;
    }
    if (!entries.empty() && selectorIndex < entries.size()) {
      onSelectBook(entries[selectorIndex].localPath);
    } else if (entries.empty()) {
      // Short-press is unambiguously the sync trigger when there's nothing to open.
      triggerSyncOrWifi();
    }
    return;
  }

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
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
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SUBSCRIPTIONS));

  // One mutex take per frame — the three service fields would otherwise each
  // acquire the mutex, and the separate acquires can interleave with the sync
  // task's state publish to give the renderer an inconsistent view.
  const auto state = SubscriptionSyncService::instance().fullSnapshot();
  const bool syncRunning = state.running;
  const auto& lastResult = state.lastResult;
  const bool showLastFailure = !syncRunning && lastResult.phase == SubscriptionSyncer::Phase::Failed;
  const bool showIdleStatus = !syncRunning && !showLastFailure && lastResult.finishedAtMs > 0;
  const bool showBanner = syncRunning || showLastFailure || showIdleStatus;

  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (showBanner) {
    int y = contentTop;

    if (syncRunning) {
      const auto& prog = state.progress;
      renderer.drawCenteredText(UI_10_FONT_ID, y, phaseLabel(prog.phase), true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;

      if (prog.phase == SubscriptionSyncer::Phase::DownloadingEpub) {
        if (prog.seriesTotal > 1) {
          char seriesBuf[32];
          snprintf(seriesBuf, sizeof(seriesBuf), tr(STR_SYNC_SERIES_PROGRESS), prog.seriesDone + 1, prog.seriesTotal);
          renderer.drawCenteredText(UI_10_FONT_ID, y, seriesBuf);
          y += lineHeight + metrics.verticalSpacing / 2;
        }
        if (!prog.currentTitle.empty()) {
          const int maxTitleWidth = pageWidth - metrics.contentSidePadding * 2;
          const std::string truncated =
              renderer.truncatedText(UI_10_FONT_ID, prog.currentTitle.c_str(), maxTitleWidth);
          renderer.drawCenteredText(UI_10_FONT_ID, y, truncated.c_str());
          y += lineHeight + metrics.verticalSpacing;
        }
        if (prog.bytesTotal > 0) {
          GUI.drawProgressBar(renderer,
                              Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2,
                                   metrics.progressBarHeight},
                              prog.bytesDone, prog.bytesTotal);
          y += metrics.progressBarHeight + metrics.verticalSpacing;
        }
      }
    } else if (showLastFailure) {
      char buf[96];
      snprintf(buf, sizeof(buf), "%s: %s", tr(STR_SYNC_FAILED), failureLabel(lastResult.failure));
      renderer.drawCenteredText(UI_10_FONT_ID, y, buf, true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
    } else {
      // Compact "Synced X ago" line so a 304-fast-path run is still visible.
      const uint32_t elapsedMs = millis() - static_cast<uint32_t>(lastResult.finishedAtMs);
      char buf[64];
      if (elapsedMs < 60UL * 1000UL) {
        snprintf(buf, sizeof(buf), "%s", tr(STR_SYNC_JUST_NOW));
      } else if (elapsedMs < 60UL * 60UL * 1000UL) {
        snprintf(buf, sizeof(buf), tr(STR_SYNC_MIN_AGO), static_cast<unsigned>(elapsedMs / 60000UL));
      } else {
        snprintf(buf, sizeof(buf), tr(STR_SYNC_HOUR_AGO), static_cast<unsigned>(elapsedMs / 3600000UL));
      }
      renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
      y += lineHeight + metrics.verticalSpacing / 2;
    }

    contentTop = y + metrics.verticalSpacing / 2;
  }

  // Subtitle only renders for the "non-empty list" case (where the short-press Open
  // is distinct from the long-press Sync). Empty-list states fit in a plain tab.
  const bool anyHintSubtitle = configured && !entries.empty();
  const int hintsHeight = GUI.getButtonHintsHeight(anyHintSubtitle);
  const int contentHeight = pageHeight - contentTop - hintsHeight - metrics.verticalSpacing;

  if (!configured) {
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

  const bool noWifiCreds = WIFI_STORE.getCredentials().empty();

  if (entries.empty()) {
    const char* msg = noWifiCreds                   ? tr(STR_SUBS_NO_WIFI)
                      : lastResult.finishedAtMs == 0 ? tr(STR_SUBS_NEVER_SYNCED)
                                                     : tr(STR_SYNC_NO_CHANGES);
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, msg);
  } else {
    // entries is sorted unread-first, so the split point is just after the last unread row.
    size_t unreadCount = 0;
    while (unreadCount < entries.size() && entries[unreadCount].unreadCount > 0) unreadCount++;
    const size_t caughtUpCount = entries.size() - unreadCount;

    const int subHeaderH = lineHeight + metrics.verticalSpacing;
    const int rowH = metrics.listRowHeight;

    auto rowTitle = [this](size_t startIdx, int i) { return entries[startIdx + i].title; };
    auto rowIcon = [this](size_t startIdx, int i) { return UITheme::getFileIcon(entries[startIdx + i].localPath); };
    auto rowValue = [this](size_t startIdx, int i) {
      const auto& e = entries[startIdx + i];
      if (e.unreadCount == 0) return std::string{};
      char buf[16];
      snprintf(buf, sizeof(buf), tr(STR_INBOX_UNREAD_COUNT), e.unreadCount);
      return std::string(buf);
    };

    auto renderSection = [&](const char* header, size_t startIdx, size_t count, int& y, int allocatedH) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, header, true, EpdFontFamily::BOLD);
      y += subHeaderH;
      const int listH = allocatedH - subHeaderH;
      const int localSel = (selectorIndex >= startIdx && selectorIndex < startIdx + count)
                               ? static_cast<int>(selectorIndex - startIdx)
                               : -1;
      GUI.drawList(
          renderer, Rect{0, y, pageWidth, listH}, static_cast<int>(count), localSel,
          [&rowTitle, startIdx](int i) { return rowTitle(startIdx, i); }, nullptr,
          [&rowIcon, startIdx](int i) { return rowIcon(startIdx, i); },
          [&rowValue, startIdx](int i) { return rowValue(startIdx, i); });
      y += listH;
    };

    int y = contentTop;
    int remainingH = contentHeight;

    if (unreadCount > 0 && caughtUpCount > 0) {
      const int unreadNaturalH = static_cast<int>(unreadCount) * rowH + subHeaderH;
      const int unreadH = std::min(unreadNaturalH, remainingH / 2);
      renderSection(tr(STR_NEW_CHAPTERS_HEADER), 0, unreadCount, y, unreadH);
      renderSection(tr(STR_INBOX_SECTION_CAUGHT_UP), unreadCount, caughtUpCount, y, remainingH - unreadH);
    } else if (unreadCount > 0) {
      renderSection(tr(STR_NEW_CHAPTERS_HEADER), 0, unreadCount, y, remainingH);
    } else {
      renderSection(tr(STR_INBOX_SECTION_CAUGHT_UP), 0, caughtUpCount, y, remainingH);
    }
  }

  // Short-press Confirm has different meanings depending on list + Wi-Fi state;
  // long-press is always "sync now" (routed through Wi-Fi setup when creds missing).
  const char* confirmLabel;
  if (!entries.empty()) {
    confirmLabel = tr(STR_OPEN);
  } else if (syncRunning) {
    confirmLabel = tr(STR_CANCEL);
  } else if (noWifiCreds) {
    confirmLabel = tr(STR_WIFI_SETUP);
  } else {
    confirmLabel = tr(STR_SYNC_NOW);
  }
  // Suppress the long-press subtitle when it would duplicate the short-press label.
  const char* confirmSubtitle = "";
  if (!entries.empty()) {
    confirmSubtitle = syncRunning    ? tr(STR_HOLD_TO_CANCEL)
                      : noWifiCreds  ? tr(STR_HOLD_TO_WIFI)
                                     : tr(STR_HOLD_TO_SYNC);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const auto subtitles = mappedInput.mapLabels("", confirmSubtitle, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, subtitles.btn1, subtitles.btn2,
                      subtitles.btn3, subtitles.btn4);

  renderer.displayBuffer();
}
