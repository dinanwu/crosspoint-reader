#include "SubscriptionSyncer.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <ctime>
#include <functional>

#include "CrossPointSettings.h"
#include "HttpDownloader.h"
#include "WifiCredentialStore.h"

namespace {
constexpr char SUBSCRIPTIONS_DIR[] = "/.subscriptions";
constexpr char EPUB_CACHE_DIR[] = "/.crosspoint";
constexpr char INDEX_ENDPOINT[] = "/v1/subs/index.json";
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint8_t SUPPORTED_FORMAT_VERSION = 1;

void syncTimeWithNTP() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  int retry = 0;
  const int maxRetries = 30;  // 3 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }
}

std::string joinUrl(const std::string& base, const std::string& path) {
  if (base.empty()) return path;
  if (path.empty()) return base;
  const bool baseSlash = base.back() == '/';
  const bool pathSlash = path.front() == '/';
  if (baseSlash && pathSlash) {
    return base + path.substr(1);
  }
  if (!baseSlash && !pathSlash) {
    return base + "/" + path;
  }
  return base + path;
}

std::string epubPathForId(const std::string& seriesId) {
  return std::string(SUBSCRIPTIONS_DIR) + "/" + seriesId + ".epub";
}

std::string partPathForId(const std::string& seriesId) { return epubPathForId(seriesId) + ".part"; }
}  // namespace

std::string SubscriptionSyncer::normalizeServerUrl(std::string url) {
  auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!url.empty() && isSpace(url.back())) url.pop_back();
  while (!url.empty() && isSpace(url.front())) url.erase(url.begin());

  auto stripTrailingSlashes = [&] {
    while (!url.empty() && url.back() == '/') url.pop_back();
  };
  stripTrailingSlashes();

  constexpr char SUFFIX[] = "/v1/subs/index.json";
  constexpr size_t suffixLen = sizeof(SUFFIX) - 1;
  if (url.size() >= suffixLen && url.compare(url.size() - suffixLen, suffixLen, SUFFIX) == 0) {
    url.resize(url.size() - suffixLen);
    stripTrailingSlashes();
  }
  return url;
}

bool SubscriptionSyncer::begin() {
  progress_ = Progress{};
  transitionTo(Phase::Idle);

  // Reset all per-run transient state — a prior Cancelled/Failed run leaves these
  // set, which would otherwise poison the next run (notably abortRequested_, which
  // would flip the very next tick() straight to Cancelled without doing any work).
  abortRequested_ = false;
  indexUnchanged_ = false;
  seriesFromIndex_.clear();
  orphanIds_.clear();
  seriesIndex_ = 0;
  orphanIndex_ = 0;
  newIndexEtag_.clear();

  if (!SETTINGS.subscriptionsEnabled) {
    LOG_DBG("SUB", "Subscriptions disabled");
    return false;
  }
  if (strlen(SETTINGS.subscriptionServerUrl) == 0 || strlen(SETTINGS.subscriptionBearerToken) == 0) {
    LOG_DBG("SUB", "Subscription URL or token not configured");
    return false;
  }
  if (WIFI_STORE.getCredentials().empty()) {
    LOG_DBG("SUB", "No Wi-Fi credentials saved");
    progress_.failure = FailureReason::NoCredentials;
    return false;
  }

  serverUrl_ = normalizeServerUrl(SETTINGS.subscriptionServerUrl);
  bearerToken_ = SETTINGS.subscriptionBearerToken;

  state_.load();  // absence is fine; fresh state persists after first successful sync

  // Ensure the subscriptions directory exists before any download tries to write
  // into it. State::save() also mkdir's, but that only runs after a successful
  // download — leaving the first-ever sync unable to open its .part file.
  Storage.mkdir(SUBSCRIPTIONS_DIR);
  return true;
}

void SubscriptionSyncer::transitionTo(Phase p) { progress_.phase = p; }

void SubscriptionSyncer::cancel() { abortRequested_ = true; }

bool SubscriptionSyncer::isTerminal() const {
  return progress_.phase == Phase::Done || progress_.phase == Phase::Failed || progress_.phase == Phase::Cancelled;
}

void SubscriptionSyncer::tick() {
  if (isTerminal()) return;

  if (abortRequested_) {
    teardownWifi();
    transitionTo(Phase::Cancelled);
    return;
  }

  switch (progress_.phase) {
    case Phase::Idle:
      transitionTo(Phase::ConnectingWifi);
      break;

    case Phase::ConnectingWifi:
      if (!connectWifi()) {
        progress_.failure = FailureReason::WifiConnect;
        transitionTo(Phase::Failed);
        return;
      }
      transitionTo(Phase::SyncingTime);
      break;

    case Phase::SyncingTime:
      syncTimeWithNTP();  // best-effort
      transitionTo(Phase::FetchingIndex);
      break;

    case Phase::FetchingIndex:
      if (!fetchAndParseIndex()) {
        teardownWifi();
        transitionTo(Phase::Failed);
        return;
      }
      if (indexUnchanged_) {
        // Two-tier 304 fast path: nothing else to do.
        teardownWifi();
        transitionTo(Phase::Done);
        return;
      }
      progress_.seriesTotal = seriesFromIndex_.size();
      progress_.seriesDone = 0;
      seriesIndex_ = 0;
      transitionTo(Phase::DownloadingEpub);
      break;

    case Phase::DownloadingEpub: {
      if (seriesIndex_ >= seriesFromIndex_.size()) {
        transitionTo(Phase::Cleaning);
        return;
      }
      progress_.currentTitle = seriesFromIndex_[seriesIndex_].title;
      progress_.bytesDone = 0;
      progress_.bytesTotal = seriesFromIndex_[seriesIndex_].size;
      if (!downloadCurrentSeries()) {
        teardownWifi();
        // If the user aborted mid-download, land on Cancelled rather than Failed.
        transitionTo(abortRequested_ ? Phase::Cancelled : Phase::Failed);
        return;
      }
      seriesIndex_++;
      progress_.seriesDone = seriesIndex_;
      break;
    }

    case Phase::Cleaning:
      cleanupOrphans();
      if (!newIndexEtag_.empty()) {
        state_.indexEtag = newIndexEtag_;
      }
      state_.save();
      teardownWifi();
      transitionTo(Phase::Done);
      break;

    case Phase::Done:
    case Phase::Failed:
    case Phase::Cancelled:
      break;
  }
}

bool SubscriptionSyncer::connectWifi() {
  // Use the last-connected SSID if we have credentials for it; otherwise take the first
  // stored credential. Simple and matches how users typically have a single home Wi-Fi.
  const auto& creds = WIFI_STORE.getCredentials();
  if (creds.empty()) return false;

  const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* target = nullptr;
  if (!lastSsid.empty()) {
    target = WIFI_STORE.findCredential(lastSsid);
  }
  if (!target) {
    target = &creds.front();
  }

  LOG_DBG("SUB", "Connecting to SSID: %s", target->ssid.c_str());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(target->ssid.c_str(), target->password.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    if (abortRequested_) return false;
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("SUB", "Wi-Fi connect timed out");
    WiFi.disconnect(true);
    return false;
  }
  LOG_DBG("SUB", "Wi-Fi connected, IP=%s", WiFi.localIP().toString().c_str());
  return true;
}

bool SubscriptionSyncer::fetchAndParseIndex() {
  const std::string url = joinUrl(serverUrl_, INDEX_ENDPOINT);
  std::string body;

  const auto result = HttpDownloader::fetchConditional(url, body, bearerToken_, state_.indexEtag);

  if (result.status == 304) {
    LOG_DBG("SUB", "Index unchanged (304)");
    indexUnchanged_ = true;
    return true;
  }

  if (result.status == 401) {
    LOG_ERR("SUB", "Index fetch: unauthorized");
    progress_.failure = FailureReason::Unauthorized;
    return false;
  }

  if (result.status != 200) {
    LOG_ERR("SUB", "Index fetch failed: HTTP %d", result.status);
    progress_.failure =
        (result.status >= 500 && result.status < 600) ? FailureReason::ServerError : FailureReason::IndexFetch;
    return false;
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR("SUB", "Index parse failed: %s", err.c_str());
    progress_.failure = FailureReason::IndexParse;
    return false;
  }

  const uint8_t version = doc["formatVersion"] | 0;
  if (version != SUPPORTED_FORMAT_VERSION) {
    LOG_ERR("SUB", "Unsupported index formatVersion: %u", version);
    progress_.failure = FailureReason::UnsupportedFormat;
    return false;
  }

  seriesFromIndex_.clear();
  JsonArrayConst series = doc["series"];
  if (series.isNull()) {
    LOG_DBG("SUB", "Index has no series array");
  } else {
    seriesFromIndex_.reserve(series.size());
    for (JsonObjectConst entry : series) {
      SeriesEntry e;
      e.id = entry["id"] | "";
      e.title = entry["title"] | "";
      e.url = entry["url"] | "";
      e.etag = entry["etag"] | "";
      e.size = entry["size"] | 0;
      e.chapterCount = entry["chapterCount"] | 0;
      if (e.id.empty() || e.url.empty()) {
        LOG_DBG("SUB", "Skipping index entry with missing id or url");
        continue;
      }
      seriesFromIndex_.push_back(std::move(e));
    }
  }

  newIndexEtag_ = result.etag;

  // Build orphan list: local series no longer present in the index.
  orphanIds_.clear();
  for (const auto& kv : state_.seriesEtags) {
    bool stillPresent = false;
    for (const auto& s : seriesFromIndex_) {
      if (s.id == kv.first) {
        stillPresent = true;
        break;
      }
    }
    if (!stillPresent) {
      orphanIds_.push_back(kv.first);
    }
  }

  LOG_DBG("SUB", "Index parsed: %u series, %u orphans", static_cast<unsigned>(seriesFromIndex_.size()),
          static_cast<unsigned>(orphanIds_.size()));
  return true;
}

bool SubscriptionSyncer::downloadCurrentSeries() {
  const SeriesEntry& series = seriesFromIndex_[seriesIndex_];
  const std::string url = joinUrl(serverUrl_, series.url);
  const std::string destPath = epubPathForId(series.id);
  const std::string partPath = partPathForId(series.id);

  // If index says the etag matches what we last stored, no need to hit the server.
  auto it = state_.seriesEtags.find(series.id);
  if (!series.etag.empty() && it != state_.seriesEtags.end() && it->second == series.etag &&
      Storage.exists(destPath.c_str())) {
    LOG_DBG("SUB", "Series '%s' unchanged by index etag, skipping", series.id.c_str());
    return true;
  }

  // Send our stored etag as If-None-Match so the server can still 304 us if it's up
  // to date. Empty string means "we don't have this series locally yet".
  const std::string ifNoneMatch = (it != state_.seriesEtags.end()) ? it->second : std::string();

  LOG_INF("SUB", "Downloading series '%s' (%zu bytes expected) url=%s ifNoneMatch=%s", series.id.c_str(), series.size,
          url.c_str(), ifNoneMatch.empty() ? "<none>" : ifNoneMatch.c_str());

  auto progressCb = [this, total = series.size](size_t downloaded, size_t reportedTotal) -> bool {
    progress_.bytesDone = downloaded;
    progress_.bytesTotal = total > 0 ? total : reportedTotal;

    // Give the UI a chance to re-render. The download runs on the SubSync task,
    // so without this hook the inbox never sees byte progress between the
    // phase-boundary publishes from taskBody().
    if (progressListener_) {
      progressListener_();
    }
    return !abortRequested_;
  };

  const auto result = HttpDownloader::downloadToFileConditional(url, partPath, bearerToken_, ifNoneMatch, progressCb);

  if (result.status == 304) {
    LOG_DBG("SUB", "Series '%s' 304 Not Modified", series.id.c_str());
    // Keep existing file and state intact; backfill metadata on first sync after
    // the state schema upgrade so the inbox can enumerate pre-existing series.
    if (state_.seriesMeta.find(series.id) == state_.seriesMeta.end() && Storage.exists(destPath.c_str())) {
      populateSeriesMeta(series.id, series.title, destPath, series.chapterCount);
    }
    return true;
  }

  if (result.status == 404) {
    LOG_DBG("SUB", "Series '%s' 404, skipping", series.id.c_str());
    return true;
  }

  if (result.status == 401) {
    LOG_ERR("SUB", "Series '%s' 401 Unauthorized", series.id.c_str());
    progress_.failure = FailureReason::Unauthorized;
    return false;
  }

  if (result.status != 200) {
    // Partial/failed transfer. Remove the .part file. If the user aborted we leave
    // progress_.failure unset so tick() can transition to Cancelled.
    if (Storage.exists(partPath.c_str())) {
      Storage.remove(partPath.c_str());
    }
    if (abortRequested_) {
      LOG_DBG("SUB", "Series '%s' download aborted by user", series.id.c_str());
      return false;
    }
    LOG_ERR("SUB", "Series '%s' download failed: %d", series.id.c_str(), result.status);
    progress_.failure =
        (result.status >= 500 && result.status < 600) ? FailureReason::ServerError : FailureReason::IndexFetch;
    return false;
  }

  // Atomic swap: .part -> .epub
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  if (!Storage.rename(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("SUB", "Failed to rename %s -> %s", partPath.c_str(), destPath.c_str());
    Storage.remove(partPath.c_str());
    progress_.failure = FailureReason::ServerError;
    return false;
  }

  // Invalidate the book.bin cache so Epub::load picks up the new spine on next open.
  // Leave sections/ and progress.bin alone.
  const std::string cachePath = SubscriptionState::cachePathForEpub(destPath);
  const std::string bookBinPath = cachePath + "/book.bin";
  if (Storage.exists(bookBinPath.c_str())) {
    Storage.remove(bookBinPath.c_str());
  }

  // Seed at 0 on first download (never overwrite an existing sidecar — the reader
  // owns it after that). A fresh subscribe reports every chapter as unread so the
  // inbox surfaces it under "New chapters". The reader suppresses the break page
  // for watermark == 0 (first-ever open), so users don't hit a "N new chapters"
  // interstitial before they've read anything.
  const std::string watermarkPath = cachePath + "/sub_watermark.bin";
  if (!Storage.exists(watermarkPath.c_str())) {
    if (SubscriptionState::writeWatermark(destPath, 0)) {
      LOG_DBG("SUB", "Seeded watermark for '%s' at 0 (chapterCount=%u)", series.id.c_str(), series.chapterCount);
    } else {
      LOG_ERR("SUB", "Failed to seed watermark for '%s'", series.id.c_str());
    }
  }

  // Persist the new etag immediately so a crash mid-sync doesn't re-download what we
  // already have.
  state_.seriesEtags[series.id] = result.etag;
  populateSeriesMeta(series.id, series.title, destPath, series.chapterCount);
  state_.save();

  progress_.anyChanges = true;
  LOG_DBG("SUB", "Series '%s' downloaded (%zu bytes)", series.id.c_str(), progress_.bytesDone);
  return true;
}

void SubscriptionSyncer::populateSeriesMeta(const std::string& seriesId, const std::string& title,
                                            const std::string& epubPath, uint16_t chapterCount) {
  SubscriptionState::SeriesMeta m;
  m.title = title;
  m.localPath = epubPath;
  // time(nullptr) returns unix seconds once NTP has synced earlier in this run.
  m.lastSyncedMs = static_cast<uint64_t>(time(nullptr)) * 1000ULL;

  if (chapterCount > 0) {
    // Fast path: the server's index already told us the spine count, so we can
    // skip the full Epub::load (~18s for a 2MB book on ESP32-C3). The server is
    // the source of truth for subscription contents per docs/subscription-sync.md.
    m.lastKnownSpineCount = chapterCount;
  } else {
    // Fallback for older servers that don't send chapterCount: parse the EPUB.
    // Keeps the inbox "N new chapters" badge working at the cost of a long stall.
    Epub epub(epubPath, EPUB_CACHE_DIR);
    if (!epub.load(true, true)) {
      LOG_ERR("SUB", "Failed to load epub for metadata: %s", epubPath.c_str());
      return;
    }
    m.lastKnownSpineCount = static_cast<uint16_t>(epub.getSpineItemsCount());
  }

  state_.seriesMeta[seriesId] = std::move(m);
}

void SubscriptionSyncer::cleanupOrphans() {
  for (const auto& orphanId : orphanIds_) {
    const std::string epubPath = epubPathForId(orphanId);
    const std::string cachePath = SubscriptionState::cachePathForEpub(epubPath);

    if (Storage.exists(epubPath.c_str())) {
      Storage.remove(epubPath.c_str());
    }
    if (Storage.exists(cachePath.c_str())) {
      Storage.removeDir(cachePath.c_str());
    }
    state_.seriesEtags.erase(orphanId);
    state_.seriesMeta.erase(orphanId);
    progress_.anyChanges = true;
    LOG_DBG("SUB", "Removed orphan series '%s'", orphanId.c_str());
  }
}

void SubscriptionSyncer::teardownWifi() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
