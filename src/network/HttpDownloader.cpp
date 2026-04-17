#include "HttpDownloader.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <base64.h>

#include <cstring>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

namespace {
// Decodes the negative error codes returned by HTTPClient::writeToStream so log
// output shows the symbolic name alongside the number. Values are from
// arduino-esp32's HTTPClient.h and must match that header.
const char* httpErrorName(int err) {
  switch (err) {
    case -1:
      return "CONNECTION_REFUSED";
    case -2:
      return "SEND_HEADER_FAILED";
    case -3:
      return "SEND_PAYLOAD_FAILED";
    case -4:
      return "NOT_CONNECTED";
    case -5:
      return "CONNECTION_LOST";
    case -6:
      return "NO_STREAM";
    case -7:
      return "NO_HTTP_SERVER";
    case -8:
      return "TOO_LESS_RAM";
    case -9:
      return "ENCODING";
    case -10:
      return "STREAM_WRITE";
    case -11:
      return "READ_TIMEOUT";
    default:
      return "UNKNOWN";
  }
}

constexpr size_t LOG_PROGRESS_EVERY_BYTES = 64 * 1024;

class FileWriteStream final : public Stream {
 public:
  FileWriteStream(FsFile& file, size_t total, HttpDownloader::ProgressCallback progress)
      : file_(file), total_(total), progress_(std::move(progress)) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Write-through stream for HTTPClient::writeToStream with progress tracking.
    if (aborted_) {
      return 0;
    }
    const size_t written = file_.write(buffer, size);
    if (written != size) {
      // Log the first short write with enough context to diagnose SD/heap trouble.
      // HTTPClient aborts with -10 on short write so later short writes don't occur.
      if (writeOk_) {
        LOG_ERR("HTTP",
                "Short SD write: requested=%zu wrote=%zu at offset=%zu / total=%zu "
                "(free=%u minFree=%u maxAlloc=%u)",
                size, written, downloaded_, total_, static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      }
      writeOk_ = false;
    }
    downloaded_ += written;

    // Periodic progress log so we can see the download actually moving during the
    // blocking HTTP read. Confirms whether a stall is in the transport or the UI.
    if (downloaded_ - lastLoggedBytes_ >= LOG_PROGRESS_EVERY_BYTES) {
      lastLoggedBytes_ = downloaded_;
      if (total_ > 0) {
        const unsigned pct = static_cast<unsigned>((static_cast<uint64_t>(downloaded_) * 100) / total_);
        LOG_INF("HTTP", "Download progress: %zu/%zu bytes (%u%%) free=%u", downloaded_, total_, pct,
                static_cast<unsigned>(ESP.getFreeHeap()));
      } else {
        LOG_INF("HTTP", "Download progress: %zu bytes free=%u", downloaded_,
                static_cast<unsigned>(ESP.getFreeHeap()));
      }
    }

    if (progress_) {
      if (!progress_(downloaded_, total_)) {
        // Caller requested cancellation. Returning 0 causes HTTPClient::writeToStream
        // to bail out with a negative result, which the caller treats as a write error.
        aborted_ = true;
        writeOk_ = false;
        return 0;
      }
    }
    return written;
  }

  bool aborted() const { return aborted_; }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }

 private:
  FsFile& file_;
  size_t total_;
  size_t downloaded_ = 0;
  size_t lastLoggedBytes_ = 0;
  bool writeOk_ = true;
  bool aborted_ = false;
  HttpDownloader::ProgressCallback progress_;
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent) {
  StreamString stream;
  if (!fetchUrl(url, stream)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  // Let HTTPClient handle chunked decoding and stream body bytes into the file.
  FileWriteStream fileStream(file, contentLength, progress);
  const int writeResult = http.writeToStream(&fileStream);

  file.close();
  http.end();

  if (writeResult < 0) {
    LOG_ERR("HTTP", "writeToStream error: %d", writeResult);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  const size_t downloaded = fileStream.downloaded();
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if HTTPClient completes.
  if (!fileStream.ok()) {
    LOG_ERR("HTTP", "Write failed during download");
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}

namespace {
// Builds an HTTPS/HTTP client pair and applies standard headers + optional bearer auth +
// optional If-None-Match. Caller owns the returned client via the out unique_ptr.
void applyConditionalHeaders(HTTPClient& http, std::unique_ptr<NetworkClient>& clientOut, const std::string& url,
                             const std::string& bearerToken, const std::string& ifNoneMatch) {
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    clientOut.reset(secureClient);
  } else {
    clientOut.reset(new NetworkClient());
  }

  http.begin(*clientOut, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!bearerToken.empty()) {
    http.addHeader("Authorization", String("Bearer ") + bearerToken.c_str());
  }
  if (!ifNoneMatch.empty()) {
    http.addHeader("If-None-Match", ifNoneMatch.c_str());
  }

  static const char* kCollectedHeaders[] = {"ETag", "Last-Modified"};
  http.collectHeaders(kCollectedHeaders, sizeof(kCollectedHeaders) / sizeof(kCollectedHeaders[0]));
}

HttpDownloader::HttpResult fillResult(HTTPClient& http, int httpCode) {
  HttpDownloader::HttpResult result;
  result.status = httpCode;
  const String etag = http.header("ETag");
  const String lastModified = http.header("Last-Modified");
  if (etag.length() > 0) {
    result.etag = etag.c_str();
  }
  if (lastModified.length() > 0) {
    result.lastModified = lastModified.c_str();
  }
  return result;
}
}  // namespace

HttpDownloader::HttpResult HttpDownloader::fetchConditional(const std::string& url, std::string& outContent,
                                                            const std::string& bearerToken,
                                                            const std::string& ifNoneMatch) {
  std::unique_ptr<NetworkClient> client;
  HTTPClient http;
  applyConditionalHeaders(http, client, url, bearerToken, ifNoneMatch);

  LOG_DBG("HTTP", "Conditional fetch: %s (etag=%s)", url.c_str(), ifNoneMatch.empty() ? "<none>" : ifNoneMatch.c_str());

  const int httpCode = http.GET();
  HttpResult result = fillResult(http, httpCode);

  if (httpCode == HTTP_CODE_OK) {
    StreamString stream;
    http.writeToStream(&stream);
    outContent = stream.c_str();
  } else if (httpCode == HTTP_CODE_NOT_MODIFIED) {
    LOG_DBG("HTTP", "304 Not Modified");
  } else {
    LOG_ERR("HTTP", "Conditional fetch failed: %d", httpCode);
  }

  http.end();
  return result;
}

HttpDownloader::HttpResult HttpDownloader::downloadToFileConditional(const std::string& url,
                                                                     const std::string& destPath,
                                                                     const std::string& bearerToken,
                                                                     const std::string& ifNoneMatch,
                                                                     ProgressCallback progress) {
  std::unique_ptr<NetworkClient> client;
  HTTPClient http;
  applyConditionalHeaders(http, client, url, bearerToken, ifNoneMatch);

  LOG_DBG("HTTP", "Conditional download: %s -> %s (etag=%s)", url.c_str(), destPath.c_str(),
          ifNoneMatch.empty() ? "<none>" : ifNoneMatch.c_str());

  const int httpCode = http.GET();
  HttpResult result = fillResult(http, httpCode);

  if (httpCode == HTTP_CODE_NOT_MODIFIED) {
    LOG_DBG("HTTP", "304 Not Modified, skipping download");
    http.end();
    return result;
  }

  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Conditional download failed: %d", httpCode);
    http.end();
    return result;
  }

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  LOG_INF("HTTP", "Begin download: %zu bytes -> %s (free=%u minFree=%u maxAlloc=%u)", contentLength, destPath.c_str(),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    result.status = -1;  // Signal file error distinct from HTTP status
    return result;
  }

  const unsigned long downloadStartMs = millis();
  FileWriteStream fileStream(file, contentLength, progress);
  const int writeResult = http.writeToStream(&fileStream);
  const unsigned long downloadElapsedMs = millis() - downloadStartMs;

  file.close();
  http.end();

  if (writeResult < 0 || !fileStream.ok()) {
    LOG_ERR("HTTP",
            "Stream write failed: writeResult=%d (%s) streamOk=%d aborted=%d "
            "downloaded=%zu/%zu elapsed=%lums (free=%u maxAlloc=%u)",
            writeResult, httpErrorName(writeResult), fileStream.ok() ? 1 : 0, fileStream.aborted() ? 1 : 0,
            fileStream.downloaded(), contentLength, downloadElapsedMs, static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    Storage.remove(destPath.c_str());
    result.status = -1;
    return result;
  }

  const size_t downloaded = fileStream.downloaded();
  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu (elapsed=%lums)", downloaded, contentLength,
            downloadElapsedMs);
    Storage.remove(destPath.c_str());
    result.status = -1;
    return result;
  }

  LOG_INF("HTTP", "Download complete: %zu bytes in %lums (free=%u)", downloaded, downloadElapsedMs,
          static_cast<unsigned>(ESP.getFreeHeap()));
  return result;
}
