# Subscription Sync — Bug Fixes & Improvements

Backlog from the post-refactor audit. The sync feature was recently moved from a blocking `SubscriptionSyncActivity` to a background `SubscriptionSyncService` FreeRTOS task — most critical items here are regressions from that refactor.

Companion docs:
- [docs/subscription-sync.md](docs/subscription-sync.md) — server wire contract
- [docs/subscription-sync-internals.md](docs/subscription-sync-internals.md) — device internals (threading, phase state machine, cache rules)

Severity key:
- **P0** — correctness regression, ship-blocker
- **P1** — functional gap or noticeable UX regression
- **P2** — polish / nice-to-have

---

## P0 — Correctness regressions

### 1. `SubscriptionSyncer::begin()` doesn't reset transient state

**File**: `src/network/SubscriptionSyncer.cpp:82-110`

`begin()` resets `progress_` and transitions to `Idle`, but leaves `abortRequested_`, `indexUnchanged_`, `seriesFromIndex_`, `orphanIds_`, `seriesIndex_`, `orphanIndex_`, and `newIndexEtag_` set from the previous run. After a user-cancel, the next sync's first `tick()` hits the `if (abortRequested_)` branch and lands on `Cancelled` without doing any work. User experiences "sync button broken after first cancel."

**Fix**: zero all transient state at the top of `begin()`:

```cpp
abortRequested_ = false;
indexUnchanged_ = false;
seriesFromIndex_.clear();
orphanIds_.clear();
seriesIndex_ = 0;
orphanIndex_ = 0;
newIndexEtag_.clear();
```

**Verify**: on device, trigger sync → long-press Confirm to cancel → wait for `Cancelled` banner → long-press again → confirm sync actually runs (wi-fi logs appear).

- [x] Fix applied
- [ ] Manually verified on hardware

---

### 2. ~~`FileWriteStream::write()` doesn't hold the HalStorage mutex~~ — NOT A BUG

**File**: `src/network/HttpDownloader.cpp:60-104` (plus `lib/hal/HalStorage.cpp`)

**Status**: false positive — the audit missed the `HalFile` wrapper.

Originally flagged as: "HalStorage's mutex is only held for the duration of the `openFileForWrite` call. Once the `FsFile` is returned, subsequent `file_.write()` calls run without any lock."

Why this is wrong:

- `lib/hal/HalStorage.h:101`: `using FsFile = HalFile;` — for all downstream code, `FsFile` is an alias for the thread-safe `HalFile` wrapper (the `HAL_STORAGE_IMPL` guard only skips this alias inside `HalStorage.cpp` itself).
- `lib/hal/HalStorage.cpp:147`: `size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }` — the macro acquires `StorageLock` (the storage mutex) for every write call.
- `src/network/HttpDownloader.cpp:65`: `file_.write(buffer, size)` — where `file_` is `FsFile&` = `HalFile&`. Every chunk write is mutex-protected.

So the raw SdFat library is never accessed concurrently. Per-chunk locking has contention implications (main task can't do SD I/O while a chunk is mid-write), but there is no correctness hazard. No code change required.

- [x] Reviewed — false positive, no fix applied

---

### 3. `gpio.update()` in HTTP progress callback races with main task

**File**: `src/network/SubscriptionSyncer.cpp:349-361`

The progress callback was correct when the sync owned the main loop. Now `gpio.update()` runs on both:

- Main task, every loop iteration (`src/main.cpp:325`).
- SubSync task, every HTTP write chunk (the callback).

The input state machine (held-time tracking, `wasPressed` / `wasReleased` edge latches) is not task-safe. Events get consumed by whichever task reads first, so the Inbox's `mappedInput.wasReleased(Button::Back)` race with the callback's `gpio.wasReleased(SETTINGS.frontButtonBack)` produces undefined behavior. Pressing Back mid-download might cancel the sync, navigate home, both, or neither.

The callback-based cancel is also redundant — `SubscriptionSyncService::cancel()` is the supported entry point and is already wired into the Inbox's long-press-Confirm handler.

**Fix**: delete lines 354-361 (the `gpio.update()` / `gpio.wasReleased()` / `abortRequested_` block). Leave the byte-level progress update and the `return !abortRequested_` at the end.

If cancel during a background sync with no Inbox visible matters (user triggered at boot, on home screen): add a Back-handler in whatever activity is foreground that calls `syncService.cancel()`. Out of scope for this task.

**Verify**: inspect `Button::Back` handling on the Inbox during a running sync — should be exactly one action (go home) without also cancelling the sync incidentally. Inversely, long-press Confirm should reliably cancel without the release being eaten by the callback.

- [x] Callback cleanup applied
- [ ] Button behavior verified on hardware

---

### 4. Progress-callback floods the render task with notifications

**File**: `src/network/SubscriptionSyncService.cpp:95-102` (via progress listener)

`publishProgress()` is called from the HTTP write callback on every TCP-sized chunk (~1500 bytes). For a 2 MB EPUB that's ~1400 calls per download. Each call:

1. Takes the service mutex and copies `Progress` (including a `std::string`).
2. Calls `activityManager.requestUpdate(true)` → `xTaskNotify(renderTaskHandle, 1, eIncrement)`.

FreeRTOS task notifications coalesce when the render task is busy, but each notification that lands on an idle render task triggers a full `render()` call which does SD reads (icons, cover art) and an e-ink `displayBuffer()`. During a download this flood amplifies the storage hazard in task 2 and visibly lags the UI.

**Fix**: throttle publishes to ~every 250 ms or ~every 2% of byte progress, whichever comes first. Add a simple time gate in the listener:

```cpp
// in SubscriptionSyncService, guarded by mutex_
uint32_t lastPublishMs_ = 0;
// in publishProgress(), before the notify:
const uint32_t now = millis();
if (syncer_.progress().phase == Phase::DownloadingEpub && now - lastPublishMs_ < 250) return;
lastPublishMs_ = now;
```

Phase transitions should bypass the throttle so "DownloadingEpub" → "Cleaning" etc. renders promptly. Easiest: track last-published phase separately and always publish on phase change.

**Verify**: during a 2 MB download, serial log shows ~10 progress-update lines instead of thousands. Inbox progress bar still animates smoothly.

- [x] Throttle applied
- [ ] Phase transitions still render promptly
- [ ] Verified on hardware

---

## P1 — UX / UI gaps

### 5. No global sync indicator outside the Subscriptions Inbox

When `startIfIdle()` fires on a PowerButton wake, the user sees Home or Reader with zero indication that Wi-Fi is on and a download is happening. The previous blocking activity made the sync visible; now it's silent background work.

**Options**:

1. **Status-row glyph on Home screen** — a tiny "syncing" icon next to the battery, visible while `SubscriptionSyncService::isRunning()`. Low-effort.
2. **Notification badge on the Subscriptions menu row** — when `lastResult.anyChanges` is true or unread count > 0.
3. **Toast on sync completion** — single-line at bottom of Home for a few seconds after a completed sync with changes.

Option 1 is the most consistent with existing patterns (battery/wifi glyphs). Option 2 is complementary.

**Files**: `src/activities/home/HomeActivity.{h,cpp}`, `src/components/themes/BaseTheme.cpp` (for header glyph).

- [ ] Design direction chosen
- [ ] Implementation
- [ ] Hardware-verified across both themes (Base + Lyra)

---

### 6. "Series N of M" progress not rendered

**File**: `src/activities/home/SubscriptionsInboxActivity.cpp:229-244`

`Progress.seriesTotal` / `seriesDone` are maintained by the syncer and `STR_SYNC_SERIES_PROGRESS: "Series %d of %d"` exists in `english.yaml:314`. Never drawn. Multi-series syncs give users no sense of where they are.

**Fix**: in the downloading banner, above the current title, render `snprintf(..., tr(STR_SYNC_SERIES_PROGRESS), seriesDone + 1, seriesTotal)` when `seriesTotal > 1`.

- [ ] Rendered
- [ ] Verified with 3+ series sync

---

### 7. "Sync" / "Cancel" button-hint subtitles are too terse

**File**: `lib/I18n/translations/english.yaml:316-317`

```
STR_HOLD_TO_SYNC: "Sync"
STR_HOLD_TO_CANCEL: "Cancel"
```

The subtitle's whole purpose is to advertise a long-press affordance. Just "Sync" doesn't say "hold". Users will short-press repeatedly and get "Open" instead.

**Fix**: change to something like `"Hold: Sync"` / `"Hold: Cancel"`. Check character budget against button-hint width (subtitles are drawn in `SMALL_FONT_ID`).

Other languages inherit the English fallback, so only `english.yaml` needs updating unless translators want to add locale-specific variants.

- [ ] Copy updated
- [ ] Fits in button-hint width in both themes

---

### 8. Inbox render acquires the service mutex three times per frame

**File**: `src/activities/home/SubscriptionsInboxActivity.cpp:212-225`

`isRunning()`, `snapshot()`, and `lastResult()` are separate calls; each takes the service mutex. Trivially consolidated.

**Fix**: add a combined accessor on `SubscriptionSyncService`:

```cpp
struct StateSnapshot {
  SubscriptionSyncer::Progress progress;
  LastResult lastResult;
  bool running;
};
StateSnapshot fullSnapshot() const;
```

Render consumes one snapshot per frame.

- [ ] Accessor added
- [ ] Render refactored

---

### 9. "No new chapters" is wrong when nothing has ever synced

**File**: `src/activities/home/SubscriptionsInboxActivity.cpp:289`

Configured + empty `entries` renders `STR_SYNC_NO_CHANGES = "No new chapters"`. But `entries` is also empty before the very first sync completes. User sees "No new chapters" when the actual state is "no syncs have ever run."

**Fix**: distinguish based on `lastResult.finishedAtMs == 0`:

- If never synced: show "Hold Confirm to sync" or similar.
- If caught up: keep "No new chapters".

Alternatively, auto-trigger `startIfIdle()` on first-ever Inbox entry when configured and never-synced. Debatable whether that's surprising behavior.

- [ ] Branch added
- [ ] New string (if needed) added to `english.yaml`

---

### 10. Long-press on Confirm has no progress feedback

A 1000 ms threshold with no visual cue leads users to release early. Fill-indicator on the Confirm button hint would communicate the affordance.

**Fix**: while `mappedInput.isPressed(Button::Confirm) && mappedInput.getHeldTime() < LONG_PRESS_SYNC_MS && !syncTriggeredByLongPress`, render a dithered progress bar across the Confirm hint background proportional to `getHeldTime() / LONG_PRESS_SYNC_MS`. Requires a new theme method or a render hook in the Inbox.

Low priority — the subtitle already hints at the affordance (once P1 task 7 lands).

- [ ] Implementation decision
- [ ] Applied (if pursued)

---

### 11. Back during a running download has ambiguous intent

On the Inbox with a sync running, Back currently triggers "go home" (per `SubscriptionsInboxActivity.cpp:142-145`). After task 3 fixes the ghost-cancel from the progress callback, the behavior is clean — but users likely expect Back to cancel the visible sync, not navigate away.

**Fix** (optional UX change): while `syncService.isRunning()`, route Back release to `syncService.cancel()` and stay on the Inbox. Require a second Back press after cancel to go home.

Not strictly a bug; flagged for UX review.

- [ ] Behavior decision
- [ ] Applied

---

## P2 — Robustness & polish

### 12. `populateSeriesMeta` timestamps are meaningless if NTP failed

**File**: `src/network/SubscriptionSyncer.cpp:451`

NTP is best-effort (`syncTimeWithNTP` has no failure path). If it fails, `time(nullptr)` returns 0 or boot-relative seconds, and `lastSyncedMs` becomes a nonsense value. The Inbox sorts by `lastSyncedMs` descending, so freshly-synced series can appear at the bottom.

**Fix**: if `time(nullptr) < 1609459200` (a post-2021 sanity threshold), fall back to a monotonic counter (e.g., increment a per-sync sequence number) or skip the timestamp update and keep the old one.

- [ ] Sanity check added

---

### 13. Stale `.part` files aren't cleaned up

**File**: `src/network/SubscriptionSyncer.cpp` (`begin()` or `FetchingIndex` branch)

If sync is interrupted mid-download (crash, power-off) and the series is later dropped from the index, the orphan cleanup removes the `.epub` but not the `.part`. Long-term accumulation of dead `.part` files on SD.

**Fix**: at the start of `FetchingIndex` (or on `begin()`), iterate `/.subscriptions/` and remove any `*.part` file older than the current sync's start time. Or unconditionally on `begin()` — the syncer only owns files in that directory.

- [ ] Sweep implemented

---

### 14. Unbounded JSON parses

**Files**: `src/network/SubscriptionSyncer.cpp:262` (index body), `src/network/SubscriptionState.cpp:64` (state file)

`JsonDocument doc;` with no size cap. With ~380 KB RAM ceiling, a malicious or buggy server response (or a corrupted state.json) could OOM. `HttpDownloader::fetchConditional` also reads the entire body into a `StreamString` with no limit.

**Fix**: cap body size in `fetchConditional` to, say, 128 KB. Cap `deserializeJson` via `DeserializationOption::Filter` or explicit size check on `body.size()`. Reject early with a clear failure reason.

- [ ] Size caps added
- [ ] New failure reason for oversized-body (optional)

---

### 15. `std::function` progress listener

**File**: `src/network/SubscriptionSyncer.h:66-67`

Per `CLAUDE.md`: `std::function<void()>` adds 2-4 KB of code per unique signature and heap-allocates the closure. Called on every HTTP write chunk.

**Fix**: replace with plain function pointer + context:

```cpp
// in SubscriptionSyncer
using ProgressListener = void (*)(void* ctx);
void setProgressListener(ProgressListener fn, void* ctx);
// store fn_ + ctx_, call fn_(ctx_) in progressCb
```

Service passes `&SubscriptionSyncService::publishProgressStatic` + `this`.

Low priority — cosmetic, not functional.

- [ ] Refactored

---

### 16. Watermark written on every reader exit, even when unchanged

**File**: `src/activities/reader/EpubReaderActivity.cpp:123-130`

Unconditional write. If the user opens/closes a subscription book without changing pages, we write the same value repeatedly. Not wear-critical on SD but aligns with the SPIFFS throttling principle.

**Fix**:

```cpp
if (isSubscription && epub) {
  const uint16_t spineCount = static_cast<uint16_t>(epub->getSpineItemsCount());
  if (spineCount != watermarkSpineCount) {
    SubscriptionState::writeWatermark(epub->getPath(), spineCount);
    watermarkSpineCount = spineCount;
  }
}
```

- [ ] Skip applied

---

### 17. `chapterCount` ingest has no sanity check

**File**: `src/network/SubscriptionSyncer.cpp:290`

`e.chapterCount = entry["chapterCount"] | 0;` — truncates to `uint16_t` silently. A server sending `chapterCount: 70000` wraps to `4464`.

**Fix**: clamp to a sane ceiling (e.g., 10000) with a warning log. Or parse as `uint32_t`, check against `UINT16_MAX`, log-and-skip if exceeded.

- [ ] Clamp added

---

### 18. Auto-advance target existence not verified

**File**: `src/activities/reader/EpubReaderActivity.cpp:983-991`

`tryAutoAdvanceToNextSubscription` picks the top unread candidate from state and calls `goToReader(localPath)` without checking the file exists. If cleanup removed it between the state snapshot and the advance (e.g., a concurrent sync unsubscribed it), the reader crash-loops via `APP_STATE.readerActivityLoadCount`.

**Fix**: `if (!Storage.exists(it->second.localPath.c_str())) return false;` before `goToReader`.

- [ ] Check added

---

### 19. `lastSyncedMs` vs `finishedAtMs` semantic conflation

Two different fields use the same name pattern:

- `SubscriptionState::SeriesMeta::lastSyncedMs` — wall-clock Unix ms from `time(nullptr) * 1000`.
- `SubscriptionSyncService::LastResult::finishedAtMs` — `millis()` since boot.

The Inbox's "Synced X ago" calculation uses `millis() - finishedAtMs` which is correct, but a future reader of the code could easily cross-wire these two.

**Fix** (documentation-only, no behavior change): rename `finishedAtMs` to `finishedAtBootMs` or add a doc comment distinguishing the two. Optional since the internals doc already explains this.

- [ ] Rename / comment added

---

## Doc follow-ups (optional)

- [ ] Add screenshots or mockups to `docs/subscription-sync-internals.md` UX section.
- [ ] Once P0 tasks 1-4 are fixed, delete the corresponding entries from the "Known issues" section of `docs/subscription-sync-internals.md`.
- [ ] When the P1 status-indicator (task 5) lands, update the internals doc's "UX surface map" to show it.

---

## Suggested execution order

1. **P0 as a batch** (tasks 1-4) — ship together as one regression-fix PR. Commit per task for bisectability.
2. **P1 UX batch** (tasks 7, 8, 6, 9 in that order) — each small, can land independently.
3. **P1 task 5** (global sync indicator) — standalone, needs design input.
4. **P1 task 10, 11** — UX review required before implementation.
5. **P2** — pick off individually as time permits.

Test checklist for any P0/P1 PR:
- [ ] Build clean on `default`, `gh_release`, `slim`.
- [ ] `pio check` clean.
- [ ] Manual sync on hardware — successful run.
- [ ] Manual cancel mid-download — lands on `Cancelled`.
- [ ] Re-trigger sync after cancel — actually runs (task 1 regression).
- [ ] Concurrent reader use during sync — no SD errors (task 2 regression).
- [ ] Button hint rendering — both themes, both device variants (X3 / X4).
