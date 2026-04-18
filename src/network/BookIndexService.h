#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class Epub;

// Owns a FreeRTOS task that finishes building an EPUB's metadata cache in the
// background so the reader can render page 0 from a minimal in-RAM spine while
// the heavier work (TOC parse, ZIP-size scan, buildBookBin, CSS parse) runs off
// the main thread.
//
// Lifecycle:
//   - start(epub) spawns the task if one isn't already running for some book.
//   - The task holds a HalPowerManager::Lock so the CPU doesn't throttle to
//     10MHz during the read window.
//   - On completion the task calls epub->onBackgroundCacheBuilt(success) so the
//     reader swaps its minimal cache for the freshly-written book.bin.
//   - cancel() sets an abort flag that the task checks at phase boundaries.
//   - The Epub destructor MUST call cancelAndWait() so the task doesn't outlive
//     its Epub pointer.
class BookIndexService {
 public:
  static BookIndexService& instance();

  // Returns false if another build is already running. Caller keeps the Epub*
  // alive for the lifetime of the task; see cancelAndWait().
  bool start(Epub* epub);

  // Set the abort flag. Returns immediately; the task exits at the next phase
  // boundary.
  void cancel();

  // Set the abort flag and busy-wait until the task has exited. Safe to call
  // from the Epub destructor; blocks up to ~3s in the worst case (mid-ZIP scan).
  void cancelAndWait(const Epub* epub);

  bool isRunning() const;

  bool abortRequested() const;

 private:
  BookIndexService();
  BookIndexService(const BookIndexService&) = delete;
  BookIndexService& operator=(const BookIndexService&) = delete;

  static void taskTrampoline(void* ctx);
  void taskBody();

  Epub* epub_ = nullptr;          // guarded by mutex_, only valid while running_
  bool running_ = false;          // guarded by mutex_
  bool abortRequested_ = false;   // guarded by mutex_
  SemaphoreHandle_t mutex_ = nullptr;
};
