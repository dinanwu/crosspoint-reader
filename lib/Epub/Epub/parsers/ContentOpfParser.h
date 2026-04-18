#pragma once
#include <Print.h>

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  // When true, the manifest/spine are resolved entirely in RAM (see inMemoryItemMap,
  // inMemorySpineHrefs). tempItemStore stays closed and no .items.bin file is written.
  bool inMemoryMode;
  FsFile tempItemStore;
  std::unordered_map<std::string, std::string> inMemoryItemMap;  // idref → href, used only in inMemoryMode
  std::string coverItemId;

  // Index for fast idref→href lookup (used only for large EPUBs)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::deque<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;

  // Always use the sorted idref index + binary search for spine resolution. The fallback
  // linear-scan of .items.bin is O(n²) on SD (200ms per item at 1500 items per the inline
  // comment at the fallback site); the sorted index is cheaper at every scale.
  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 0;

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  // In-memory output: populated when inMemoryMode is true, in manifest/spine order.
  // Caller moves this into BookMetadataCache::becomeMinimal() after parsing.
  std::vector<std::string> inMemorySpineHrefs;

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache, const bool inMemoryMode = false)
      : cachePath(cachePath),
        baseContentPath(baseContentPath),
        remainingSize(xmlSize),
        cache(cache),
        inMemoryMode(inMemoryMode) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
