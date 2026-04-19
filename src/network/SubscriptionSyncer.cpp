#include "SubscriptionSyncer.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <cstring>
#include <ctime>
#include <unordered_set>

#include "CrossPointSettings.h"
#include "HttpDownloader.h"
#include "WifiCredentialStore.h"

namespace {
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
  if (baseSlash && pathSlash) return base + path.substr(1);
  if (!baseSlash && !pathSlash) return base + "/" + path;
  return base + path;
}
}  // namespace

std::string SubscriptionSyncer::normalizeServerUrl(std::string url) {
  auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!url.empty() && isSpace(url.back())) url.pop_back();
  while (!url.empty() && isSpace(url.front())) url.erase(url.begin());

  auto stripTrailingSlashes = [&] {
    while (!url.empty() && url.back() == '/') url.pop_back();
  };
  stripTrailingSlashes();

  const char* suffix = SubscriptionState::INDEX_ENDPOINT;
  const size_t suffixLen = std::strlen(suffix);
  if (url.size() >= suffixLen && url.compare(url.size() - suffixLen, suffixLen, suffix) == 0) {
    url.resize(url.size() - suffixLen);
    stripTrailingSlashes();
  }
  return url;
}

bool SubscriptionSyncer::begin() {
  progress_ = Progress{};
  transitionTo(Phase::Idle);

  // Clear all per-run transient state — a prior Cancelled/Failed run would
  // otherwise leave abortRequested_ set and terminate the next run immediately.
  abortRequested_ = false;
  indexUnchanged_ = false;
  seriesFromIndex_.clear();
  orphanIds_.clear();
  seriesIndex_ = 0;
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
    return false;
  }

  serverUrl_ = normalizeServerUrl(SETTINGS.subscriptionServerUrl);
  bearerToken_ = SETTINGS.subscriptionBearerToken;

  state_.load();  // absence is fine; fresh state persists after first successful sync

  // State::save() mkdir's, but it only runs after a successful download — the
  // first-ever sync would otherwise fail to open its .part file.
  Storage.mkdir(SubscriptionState::SUBSCRIPTIONS_DIR);
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
        // connectWifi() left the radio in STA mode; tear it down so a failed
        // connect doesn't leak the Wi-Fi radio until next reboot.
        teardownWifi();
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
  const std::string url = joinUrl(serverUrl_, SubscriptionState::INDEX_ENDPOINT);
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

  std::unordered_set<std::string> freshIds;
  freshIds.reserve(seriesFromIndex_.size());
  for (const auto& s : seriesFromIndex_) freshIds.insert(s.id);

  orphanIds_.clear();
  for (const auto& kv : state_.seriesEtags) {
    if (freshIds.find(kv.first) == freshIds.end()) orphanIds_.push_back(kv.first);
  }

  LOG_DBG("SUB", "Index parsed: %u series, %u orphans", static_cast<unsigned>(seriesFromIndex_.size()),
          static_cast<unsigned>(orphanIds_.size()));
  return true;
}

bool SubscriptionSyncer::downloadCurrentSeries() {
  const SeriesEntry& series = seriesFromIndex_[seriesIndex_];
  const std::string url = joinUrl(serverUrl_, series.url);
  const std::string destPath = SubscriptionState::epubPathForId(series.id);
  const std::string partPath = SubscriptionState::partPathForId(series.id);

  auto it = state_.seriesEtags.find(series.id);
  if (!series.etag.empty() && it != state_.seriesEtags.end() && it->second == series.etag &&
      Storage.exists(destPath.c_str())) {
    LOG_DBG("SUB", "Series '%s' unchanged by index etag, skipping", series.id.c_str());
    // Backfill metadata for state.json v1 files, which stored seriesEtags but
    // no seriesMeta — the inbox would otherwise hide the series until the
    // server changed its etag.
    if (state_.seriesMeta.find(series.id) == state_.seriesMeta.end()) {
      populateSeriesMeta(series, destPath);
    }
    return true;
  }

  const std::string ifNoneMatch = (it != state_.seriesEtags.end()) ? it->second : std::string();

  LOG_INF("SUB", "Downloading series '%s' (%zu bytes expected) url=%s ifNoneMatch=%s", series.id.c_str(), series.size,
          url.c_str(), ifNoneMatch.empty() ? "<none>" : ifNoneMatch.c_str());

  auto progressCb = [this, total = series.size](size_t downloaded, size_t reportedTotal) -> bool {
    progress_.bytesDone = downloaded;
    progress_.bytesTotal = total > 0 ? total : reportedTotal;
    if (progressListener_) progressListener_(progressCtx_);
    return !abortRequested_;
  };

  const auto result = HttpDownloader::downloadToFileConditional(url, partPath, bearerToken_, ifNoneMatch, progressCb);

  if (result.status == 304) {
    LOG_DBG("SUB", "Series '%s' 304 Not Modified", series.id.c_str());
    if (state_.seriesMeta.find(series.id) == state_.seriesMeta.end() && Storage.exists(destPath.c_str())) {
      populateSeriesMeta(series, destPath);
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
    Storage.remove(partPath.c_str());
    if (abortRequested_) {
      LOG_DBG("SUB", "Series '%s' download aborted by user", series.id.c_str());
      return false;
    }
    LOG_ERR("SUB", "Series '%s' download failed: %d", series.id.c_str(), result.status);
    progress_.failure =
        (result.status >= 500 && result.status < 600) ? FailureReason::ServerError : FailureReason::IndexFetch;
    return false;
  }

  // Atomic swap: .part -> .epub. remove(destPath) is a no-op when missing.
  Storage.remove(destPath.c_str());
  if (!Storage.rename(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("SUB", "Failed to rename %s -> %s", partPath.c_str(), destPath.c_str());
    Storage.remove(partPath.c_str());
    progress_.failure = FailureReason::ServerError;
    return false;
  }

  // Invalidate book.bin so Epub::load reparses the new spine on next open.
  // sections/ and progress.bin stay — chapter N's layout is still valid under
  // the server's append-only, stable-spine-index invariant.
  const std::string cachePath = SubscriptionState::cachePathForEpub(destPath);
  Storage.remove((cachePath + "/book.bin").c_str());

  // Seed watermark at 0 only if absent — the reader owns the sidecar after first open,
  // and a fresh subscribe should surface every chapter as unread.
  if (!SubscriptionState::isSubscription(destPath)) {
    if (SubscriptionState::writeWatermark(destPath, 0)) {
      LOG_DBG("SUB", "Seeded watermark for '%s' at 0 (chapterCount=%u)", series.id.c_str(), series.chapterCount);
    } else {
      LOG_ERR("SUB", "Failed to seed watermark for '%s'", series.id.c_str());
    }
  }

  // Persist etag before advancing — a mid-sync crash won't force re-downloads.
  state_.seriesEtags[series.id] = result.etag;
  populateSeriesMeta(series, destPath);
  state_.save();

  progress_.anyChanges = true;
  LOG_DBG("SUB", "Series '%s' downloaded (%zu bytes)", series.id.c_str(), progress_.bytesDone);
  return true;
}

void SubscriptionSyncer::populateSeriesMeta(const SeriesEntry& series, const std::string& epubPath) {
  SubscriptionState::SeriesMeta m;
  m.title = series.title;
  m.localPath = epubPath;
  // time(nullptr) returns unix seconds once NTP has synced earlier in this run.
  m.lastSyncedMs = static_cast<uint64_t>(time(nullptr)) * 1000ULL;

  if (series.chapterCount > 0) {
    // Fast path per docs/subscription-sync.md: server is the source of truth for
    // spine count, so we skip Epub::load (~18s for a 2MB book on ESP32-C3).
    m.lastKnownSpineCount = series.chapterCount;
  } else {
    // Fallback for older servers that omit chapterCount.
    Epub epub(epubPath, SubscriptionState::EPUB_CACHE_DIR);
    if (!epub.load(true, true)) {
      LOG_ERR("SUB", "Failed to load epub for metadata: %s", epubPath.c_str());
      return;
    }
    m.lastKnownSpineCount = static_cast<uint16_t>(epub.getSpineItemsCount());
  }

  state_.seriesMeta[series.id] = std::move(m);
}

void SubscriptionSyncer::cleanupOrphans() {
  for (const auto& orphanId : orphanIds_) {
    const std::string epubPath = SubscriptionState::epubPathForId(orphanId);
    Storage.remove(epubPath.c_str());
    Storage.removeDir(SubscriptionState::cachePathForEpub(epubPath).c_str());
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
