#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "SubscriptionState.h"

// Step-at-a-time state machine driving an end-to-end subscription sync. The
// owning task calls tick() in a loop and polls progress() between phases.
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
  // should not run; the caller should simply finish() in that case.
  bool begin();

  // Fires whenever progress_ advances during a blocking per-series download. Without
  // it the progress bar would freeze until tick() returns from HTTPClient::writeToStream.
  using ProgressListener = void (*)(void* ctx);
  void setProgressListener(ProgressListener fn, void* ctx) {
    progressListener_ = fn;
    progressCtx_ = ctx;
  }

  // Advances one step. Safe to call while isTerminal() — no-op.
  void tick();

  // Request termination. tick() observes this at phase boundaries and exits cleanly.
  void cancel();

  const Progress& progress() const { return progress_; }
  bool isTerminal() const;

  // Trims whitespace, trailing slashes, and a trailing INDEX_ENDPOINT suffix so
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
  bool downloadCurrentSeries();
  void cleanupOrphans();
  void teardownWifi();
  void populateSeriesMeta(const SeriesEntry& series, const std::string& epubPath);

  Progress progress_;
  SubscriptionState state_;
  std::string serverUrl_;
  std::string bearerToken_;
  std::vector<SeriesEntry> seriesFromIndex_;
  std::vector<std::string> orphanIds_;  // local IDs no longer in index
  size_t seriesIndex_ = 0;
  bool indexUnchanged_ = false;
  bool abortRequested_ = false;
  std::string newIndexEtag_;
  ProgressListener progressListener_ = nullptr;
  void* progressCtx_ = nullptr;
};
