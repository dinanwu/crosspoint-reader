#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "SubscriptionState.h"

// Drives the end-to-end subscription sync as a step-at-a-time state machine so the
// owning activity can poll progress between phases and render status updates.
//
// Typical flow from an activity's loop():
//   if (!syncer.isTerminal()) {
//     syncer.tick();
//     requestUpdate();
//   } else {
//     finish();
//   }
class SubscriptionSyncer {
 public:
  enum class Phase : uint8_t {
    Idle,
    ConnectingWifi,
    SyncingTime,
    FetchingIndex,
    DownloadingEpub,
    Cleaning,
    Done,
    Failed,
    Cancelled,
  };

  enum class FailureReason : uint8_t {
    None,
    NoCredentials,
    WifiConnect,
    IndexFetch,
    IndexParse,
    UnsupportedFormat,
    Unauthorized,
    ServerError,
  };

  struct Progress {
    Phase phase = Phase::Idle;
    std::string currentTitle;
    uint16_t seriesDone = 0;
    uint16_t seriesTotal = 0;
    size_t bytesDone = 0;
    size_t bytesTotal = 0;
    FailureReason failure = FailureReason::None;
    // True once sync has applied changes (download or deletion) beyond conditional-GET no-ops.
    bool anyChanges = false;
  };

  // Validates preconditions (enabled, URL, token, creds). Returns false if the sync
  // should not run; the activity should simply finish() in that case.
  bool begin();

  // Fires whenever progress_ has been updated during a blocking per-series download.
  // The owning activity uses this to request a re-render while tick() is stuck
  // inside HTTPClient::writeToStream — without it the progress bar is frozen on
  // whatever state was visible when the download started.
  using ProgressListener = std::function<void()>;
  void setProgressListener(ProgressListener listener) { progressListener_ = std::move(listener); }

  // Advances one step. Safe to call while isTerminal() — no-op.
  void tick();

  // Request termination. tick() observes this at phase boundaries and exits cleanly.
  void cancel();

  const Progress& progress() const { return progress_; }
  bool isTerminal() const;

  // Trims whitespace, trailing slashes, and a trailing "/v1/subs/index.json" suffix so
  // users who paste the full endpoint URL from the docs still get a valid base.
  static std::string normalizeServerUrl(std::string url);

 private:
  struct SeriesEntry {
    std::string id;
    std::string title;
    std::string url;
    std::string etag;
    size_t size = 0;
    uint16_t chapterCount = 0;
  };

  void transitionTo(Phase p);
  bool connectWifi();
  bool fetchAndParseIndex();
  // Downloads the series at seriesIndex_. Returns true on success (or intentional skip);
  // sets failure and returns false on terminal error for the whole sync.
  bool downloadCurrentSeries();
  void cleanupOrphans();
  void teardownWifi();
  // Writes SeriesMeta for the given series. When chapterCount > 0 the index already
  // told us how many spine items the EPUB has, so we can skip Epub::load entirely —
  // otherwise we fall back to parsing the EPUB (expensive: ~18s for a 2MB file).
  void populateSeriesMeta(const std::string& seriesId, const std::string& title, const std::string& epubPath,
                          uint16_t chapterCount);

  Progress progress_;
  SubscriptionState state_;
  std::string serverUrl_;
  std::string bearerToken_;
  std::vector<SeriesEntry> seriesFromIndex_;
  std::vector<std::string> orphanIds_;  // local IDs no longer in index
  size_t seriesIndex_ = 0;
  size_t orphanIndex_ = 0;
  bool indexUnchanged_ = false;
  bool abortRequested_ = false;
  std::string newIndexEtag_;
  ProgressListener progressListener_;
};
