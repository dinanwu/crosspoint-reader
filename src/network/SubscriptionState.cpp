#include "SubscriptionState.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <functional>

namespace {
// Bumped from 1 → 2 when seriesMeta was added. Older state files load in a
// best-effort way: etags are preserved but seriesMeta stays empty until the
// next successful download repopulates it.
constexpr uint8_t STATE_FORMAT_VERSION = 2;
constexpr char SUBSCRIPTIONS_DIR[] = "/.subscriptions";
constexpr char EPUB_CACHE_DIR[] = "/.crosspoint";
}  // namespace

constexpr char SubscriptionState::STATE_FILE_PATH[];

std::string SubscriptionState::cachePathForEpub(const std::string& epubPath) {
  return std::string(EPUB_CACHE_DIR) + "/epub_" + std::to_string(std::hash<std::string>{}(epubPath));
}

uint16_t SubscriptionState::readWatermark(const std::string& epubPath) {
  const std::string path = cachePathForEpub(epubPath) + "/sub_watermark.bin";
  FsFile f;
  if (!Storage.openFileForRead("SUB", path, f)) {
    return 0;
  }
  uint8_t buf[2] = {0, 0};
  const int n = f.read(buf, 2);
  f.close();
  if (n != 2) return 0;
  return static_cast<uint16_t>(buf[0] | (buf[1] << 8));
}

bool SubscriptionState::writeWatermark(const std::string& epubPath, uint16_t spineCount) {
  const std::string cacheDir = cachePathForEpub(epubPath);
  Storage.mkdir(cacheDir.c_str());
  const std::string path = cacheDir + "/sub_watermark.bin";
  FsFile f;
  if (!Storage.openFileForWrite("SUB", path, f)) {
    return false;
  }
  const uint8_t buf[2] = {static_cast<uint8_t>(spineCount & 0xff), static_cast<uint8_t>((spineCount >> 8) & 0xff)};
  f.write(buf, 2);
  f.close();
  return true;
}

bool SubscriptionState::load() {
  if (!Storage.exists(STATE_FILE_PATH)) {
    LOG_DBG("SUB", "No subscription state file; starting fresh");
    return false;
  }

  String raw = Storage.readFile(STATE_FILE_PATH);
  if (raw.isEmpty()) {
    LOG_ERR("SUB", "Empty subscription state file");
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, raw.c_str());
  if (err) {
    LOG_ERR("SUB", "Failed to parse state.json: %s", err.c_str());
    return false;
  }

  const uint8_t version = doc["formatVersion"] | 0;
  if (version != 1 && version != STATE_FORMAT_VERSION) {
    LOG_ERR("SUB", "Unsupported state format version: %u", version);
    return false;
  }

  indexEtag = doc["indexEtag"] | "";

  seriesEtags.clear();
  JsonObjectConst etagMap = doc["seriesEtags"];
  if (!etagMap.isNull()) {
    for (JsonPairConst kv : etagMap) {
      const char* etag = kv.value().as<const char*>();
      if (etag) {
        seriesEtags.emplace(kv.key().c_str(), etag);
      }
    }
  }

  seriesMeta.clear();
  JsonObjectConst metaMap = doc["seriesMeta"];
  if (!metaMap.isNull()) {
    for (JsonPairConst kv : metaMap) {
      JsonObjectConst entry = kv.value().as<JsonObjectConst>();
      if (entry.isNull()) continue;
      SeriesMeta m;
      m.title = entry["title"] | "";
      m.localPath = entry["localPath"] | "";
      m.lastSyncedMs = entry["lastSyncedMs"] | 0ULL;
      m.lastKnownSpineCount = entry["lastKnownSpineCount"] | 0;
      seriesMeta.emplace(kv.key().c_str(), std::move(m));
    }
  }

  LOG_DBG("SUB", "Loaded state: indexEtag=%s, %u series, %u meta", indexEtag.c_str(),
          static_cast<unsigned>(seriesEtags.size()), static_cast<unsigned>(seriesMeta.size()));
  return true;
}

bool SubscriptionState::save() const {
  Storage.mkdir(SUBSCRIPTIONS_DIR);

  JsonDocument doc;
  doc["formatVersion"] = STATE_FORMAT_VERSION;
  doc["indexEtag"] = indexEtag;

  JsonObject etagMap = doc["seriesEtags"].to<JsonObject>();
  for (const auto& kv : seriesEtags) {
    etagMap[kv.first] = kv.second;
  }

  JsonObject metaMap = doc["seriesMeta"].to<JsonObject>();
  for (const auto& kv : seriesMeta) {
    JsonObject entry = metaMap[kv.first].to<JsonObject>();
    entry["title"] = kv.second.title;
    entry["localPath"] = kv.second.localPath;
    entry["lastSyncedMs"] = kv.second.lastSyncedMs;
    entry["lastKnownSpineCount"] = kv.second.lastKnownSpineCount;
  }

  String out;
  serializeJson(doc, out);
  if (!Storage.writeFile(STATE_FILE_PATH, out)) {
    LOG_ERR("SUB", "Failed to write state.json");
    return false;
  }
  return true;
}

std::vector<std::string> SubscriptionState::unreadSeriesIds(const std::string& excludedId) const {
  struct Candidate {
    std::string id;
    uint64_t lastSyncedMs;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(seriesMeta.size());

  for (const auto& kv : seriesMeta) {
    if (!excludedId.empty() && kv.first == excludedId) continue;
    if (kv.second.localPath.empty()) continue;
    const uint16_t watermark = readWatermark(kv.second.localPath);
    if (watermark < kv.second.lastKnownSpineCount) {
      candidates.push_back({kv.first, kv.second.lastSyncedMs});
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.lastSyncedMs > b.lastSyncedMs; });

  std::vector<std::string> result;
  result.reserve(candidates.size());
  for (auto& c : candidates) {
    result.push_back(std::move(c.id));
  }
  return result;
}
