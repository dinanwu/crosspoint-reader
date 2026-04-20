#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps NetworkClientSecure and HTTPClient for HTTPS requests.
 */
class HttpDownloader {
 public:
  // Return true to continue, false to abort the transfer.
  // For long downloads the callback is the only mechanism the caller has to service input
  // and user cancellation while the blocking HTTP read is in progress.
  using ProgressCallback = std::function<bool(size_t downloaded, size_t total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent);

  static bool fetchUrl(const std::string& url, Stream& stream);

  /**
   * Download a file to the SD card.
   * @param url The URL to download
   * @param destPath The destination path on SD card
   * @param progress Optional progress callback
   * @return DownloadError indicating success or failure type
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr);

  // Response metadata returned by conditional-GET helpers. `status` carries the HTTP code
  // (0 or negative on transport failure; 200/206/304 are the expected success codes).
  // When the caller requests a byte range and the server honors it with 206, rangeStart
  // and rangeTotalSize echo back the parsed Content-Range header so the caller knows
  // the file was updated in place instead of fully rewritten.
  struct HttpResult {
    int status = 0;
    std::string etag;
    size_t rangeStart = 0;      // First byte written. 0 on 200 or 304.
    size_t rangeTotalSize = 0;  // Full-resource size from Content-Range. 0 on 200 or 304.
  };

  /**
   * Conditional GET of a text resource into memory.
   * @param url The URL to fetch
   * @param outContent Filled with body when status is 200; untouched on 304 or error
   * @param bearerToken If non-empty, sent as Authorization: Bearer <token>
   * @param ifNoneMatch If non-empty, sent as If-None-Match: <etag>
   */
  static HttpResult fetchConditional(const std::string& url, std::string& outContent, const std::string& bearerToken,
                                     const std::string& ifNoneMatch);

  /**
   * Conditional GET that streams a 200 or 206 response body to a file. On 304 or error
   * the file is not created (or is removed if partially written on a full download).
   *
   * When `rangeStart > 0` the request includes `Range: bytes=<rangeStart>-`. If the
   * server honors it with `206 Partial Content`, `destPath` MUST already exist and is
   * modified in place: bytes at `[rangeStart, EOF)` are overwritten and the file is
   * truncated to the new end. If the server declines and replies `200 OK`, the file
   * is rewritten from scratch (full-download fallback) and `HttpResult.rangeStart` is 0.
   *
   * @param url The URL to download
   * @param destPath Destination path on SD
   * @param bearerToken If non-empty, sent as Authorization: Bearer <token>
   * @param ifNoneMatch If non-empty, sent as If-None-Match: <etag>
   * @param rangeStart If > 0, sent as Range: bytes=<rangeStart>-; 206 handled in place
   * @param progress Optional progress callback
   */
  static HttpResult downloadToFileConditional(const std::string& url, const std::string& destPath,
                                              const std::string& bearerToken, const std::string& ifNoneMatch,
                                              size_t rangeStart = 0, ProgressCallback progress = nullptr);
};
