#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Persistent state for subscription sync. Stored at /.subscriptions/state.json.
// Holds ETags for conditional GETs plus per-series metadata (title, local path,
// last sync timestamp, and spine count at last sync) so the inbox and
// auto-advance can enumerate unread series without re-parsing every EPUB.
class SubscriptionState {
 public:
  static constexpr char SUBSCRIPTIONS_DIR[] = "/.subscriptions";
  static constexpr char EPUB_CACHE_DIR[] = "/.crosspoint";
  static constexpr char STATE_FILE_PATH[] = "/.subscriptions/state.json";
  static constexpr char INDEX_ENDPOINT[] = "/v1/subs/index.json";
  static constexpr char WATERMARK_FILENAME[] = "sub_watermark.bin";

  struct SeriesMeta {
    std::string title;
    std::string localPath;
    uint64_t lastSyncedMs = 0;
    uint16_t lastKnownSpineCount = 0;
  };

  std::string indexEtag;
  std::map<std::string, std::string> seriesEtags;
  std::map<std::string, SeriesMeta> seriesMeta;

  bool load();
  bool save() const;

  // Returns series ids with unread chapters (watermark < lastKnownSpineCount),
  // ordered by lastSyncedMs descending. Pass an id to exclude (e.g. the currently
  // open book) to skip it.
  std::vector<std::string> unreadSeriesIds(const std::string& excludedId = {}) const;

  // Returns the series id whose seriesMeta.localPath matches, or "" if none.
  std::string findIdByLocalPath(const std::string& epubPath) const;

  static std::string epubPathForId(const std::string& seriesId);
  static std::string partPathForId(const std::string& seriesId);

  // Returns the watermark spine count stored alongside an EPUB's cache dir.
  // Returns 0 if the sidecar file is missing (never-opened book).
  static uint16_t readWatermark(const std::string& epubPath);

  // Writes the watermark sidecar. Creates the cache directory if missing, so this
  // is safe to call during first-download seeding before the book has been opened.
  static bool writeWatermark(const std::string& epubPath, uint16_t spineCount);

  // True if the EPUB at this path has a watermark sidecar (i.e. it's a subscription).
  static bool isSubscription(const std::string& epubPath);

  // Computes the cache directory path used by the Epub class for a given EPUB.
  // Mirrors Epub's cachePath hashing (std::hash<std::string>{}(filepath)).
  static std::string cachePathForEpub(const std::string& epubPath);

  static std::string watermarkPathForEpub(const std::string& epubPath);
};
