#include "SubscriptionSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <string>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// How long to keep the success/failure screen up before auto-dismissing.
constexpr unsigned long TERMINAL_DISPLAY_MS = 1200;

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

void SubscriptionSyncActivity::onEnter() {
  Activity::onEnter();

  // WifiCredentialStore is loaded on demand elsewhere; load it here so syncer.begin()
  // can see saved credentials.
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  // Request a render from inside the blocking per-series download whenever the
  // percent ticks up. The render task runs on its own FreeRTOS task so it can
  // pick up these notifications even while the main task is stuck in writeToStream.
  // Throttling by percent keeps the eink refresh rate sane (≤100 renders per file).
  // `immediate=true` notifies the render task directly — the default deferred path
  // only sets a flag that ActivityManager::loop() reads, and loop() won't run again
  // until the download finishes.
  syncer.setProgressListener([this] {
    const auto& prog = syncer.progress();
    if (prog.phase != SubscriptionSyncer::Phase::DownloadingEpub || prog.bytesTotal == 0) return;
    const int pct = static_cast<int>((prog.bytesDone * 100) / prog.bytesTotal);
    if (pct != lastRenderedPercent) {
      lastRenderedPercent = pct;
      requestUpdate(true);
    }
  });

  running = syncer.begin();
  if (!running) {
    // Preconditions not met (disabled, no URL/token, no Wi-Fi creds). Silently exit
    // so the natural home/reader dispatch takes over.
    finish();
    return;
  }
  requestUpdate();
}

void SubscriptionSyncActivity::onExit() {
  // Belt-and-braces — the syncer cleans up Wi-Fi itself, but if we were torn down
  // mid-run (e.g. user forced sleep) make sure the radio is off.
  if (running && !syncer.isTerminal()) {
    syncer.cancel();
  }
  WiFi.disconnect(false);
  delay(50);
  WiFi.mode(WIFI_OFF);
  Activity::onExit();
}

bool SubscriptionSyncActivity::preventAutoSleep() { return running && !syncer.isTerminal(); }

void SubscriptionSyncActivity::loop() {
  if (!running) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (syncer.isTerminal()) {
      finish();
      return;
    }
    syncer.cancel();
  }

  if (!syncer.isTerminal()) {
    syncer.tick();

    // Request render when phase or user-visible progress has changed.
    const auto& prog = syncer.progress();
    int percent = -1;
    if (prog.phase == SubscriptionSyncer::Phase::DownloadingEpub && prog.bytesTotal > 0) {
      percent = static_cast<int>((prog.bytesDone * 100) / prog.bytesTotal);
    }
    if (prog.phase != lastRenderedPhase || prog.currentTitle != lastRenderedTitle || percent != lastRenderedPercent) {
      lastRenderedPhase = prog.phase;
      lastRenderedTitle = prog.currentTitle;
      lastRenderedPercent = percent;
      requestUpdate();
    }
    return;
  }

  // Terminal state.
  // Cancelled: the user explicitly pressed Back to get out — don't make them sit
  // through an extra "Cancelled" screen. Pop immediately. (The Back release that
  // triggered the cancel was consumed by the in-download progress callback, so the
  // activity's own wasReleased(Back) check at the top of loop() won't fire for it;
  // without this shortcut the only dismiss path is a second Back press or the
  // 1200ms auto-timeout, both of which can feel like the screen is stuck.)
  if (syncer.progress().phase == SubscriptionSyncer::Phase::Cancelled) {
    finish();
    return;
  }

  // Done: brief confirmation before dismiss. Failed: stay until the user presses
  // Back so they can read the failure reason.
  if (terminalEnteredAt == 0) {
    terminalEnteredAt = millis();
    requestUpdate();
    return;
  }
  if (syncer.progress().phase != SubscriptionSyncer::Phase::Failed &&
      millis() - terminalEnteredAt >= TERMINAL_DISPLAY_MS) {
    finish();
  }
}

void SubscriptionSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYNCING_SUBSCRIPTIONS));

  const auto& prog = syncer.progress();
  int y = (pageHeight - lineHeight) / 2 - lineHeight;

  using Phase = SubscriptionSyncer::Phase;

  if (prog.phase == Phase::Done) {
    const char* msg = prog.anyChanges ? tr(STR_SYNC_COMPLETE) : tr(STR_SYNC_NO_CHANGES);
    renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight, msg, true, EpdFontFamily::BOLD);
  } else if (prog.phase == Phase::Failed) {
    renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight, tr(STR_SYNC_FAILED), true, EpdFontFamily::BOLD);
    const char* reason = failureLabel(prog.failure);
    if (reason && *reason) {
      renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight * 2 + metrics.verticalSpacing, reason);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (prog.phase == Phase::Cancelled) {
    renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight, tr(STR_SYNC_CANCELLED), true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, phaseLabel(prog.phase));
    y += lineHeight + metrics.verticalSpacing;

    if (prog.phase == Phase::DownloadingEpub) {
      if (!prog.currentTitle.empty()) {
        // Title comes from the server's index.json and can be arbitrarily long.
        // drawCenteredText computes x = (screenWidth - textWidth) / 2, so an over-wide
        // title drives x negative and every glyph logs "Outside range". Truncate first.
        const int maxTitleWidth = pageWidth - metrics.contentSidePadding * 2;
        const std::string truncated =
            renderer.truncatedText(UI_10_FONT_ID, prog.currentTitle.c_str(), maxTitleWidth, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, y, truncated.c_str(), true, EpdFontFamily::BOLD);
        y += lineHeight + metrics.verticalSpacing;
      }

      // "Series N of M"
      if (prog.seriesTotal > 0) {
        char counter[64];
        snprintf(counter, sizeof(counter), tr(STR_SYNC_SERIES_PROGRESS), prog.seriesDone + 1, prog.seriesTotal);
        renderer.drawCenteredText(UI_10_FONT_ID, y, counter);
        y += lineHeight + metrics.verticalSpacing;
      }

      if (prog.bytesTotal > 0) {
        const int pct = static_cast<int>((prog.bytesDone * 100) / prog.bytesTotal);
        GUI.drawProgressBar(
            renderer,
            Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
            pct, 100);
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
