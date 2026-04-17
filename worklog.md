# Worklog — Web serial subscription sync

## What this is

Device-side implementation of automatic sync for web-serial EPUBs (Royal Road etc.) from a user-operated external server to the CrossPoint e-reader. Server produces one EPUB per series; device polls on power-button wake, downloads updates, and a small break-page interstitial marks new chapters in the reader.

See [docs/subscription-sync.md](docs/subscription-sync.md) for the full server wire contract.

## Design path

- **Shape chosen**: server builds a full EPUB per series → device treats it like any other EPUB. No reader refactor.
  - Rejected: polymorphic `SpineBook` interface refactor, per-chapter XHTML + manifest.json on device, background FreeRTOS sync task.
  - Accepted cost: full-EPUB redownload per content update (~2MB for a 500-chapter serial on home Wi-Fi is tolerable).
- **Sync trigger**: always on `PowerButton` wake. Not interval-gated. The two-tier conditional-GET 304 fast-path is the real interval.
  - Why no interval: `HalPowerManager::startDeepSleep` fully powers off the RTC, so `RTC_DATA_ATTR` doesn't survive deep sleep on battery. No reliable cross-sleep monotonic clock.
- **Activity model**: blocking `SubscriptionSyncActivity` pushed onto the stack before home/reader. Back button cancels and defers. Pattern mirrors `OtaUpdateActivity`.
- **Progress preservation invariant (server-side)**: chapters must be spine-stable. Append-only, never re-order.

## What got built

Built in 7 sequential phases, each build-verified.

| # | Phase | Files |
|---|---|---|
| 1 | Settings + i18n strings | `CrossPointSettings.h`, `SettingsList.h`, `lib/I18n/translations/english.yaml` (+ regenerated i18n headers) |
| 2 | HttpDownloader: bearer auth + conditional GET + ETag extraction | `src/network/HttpDownloader.{h,cpp}` |
| 3 | SubscriptionState — `/.subscriptions/state.json` load/save (ArduinoJson) | `src/network/SubscriptionState.{h,cpp}` (new) |
| 4 | SubscriptionSyncer — state machine (ConnectingWifi → SyncingTime → FetchingIndex → DownloadingEpub → Cleaning → Done) | `src/network/SubscriptionSyncer.{h,cpp}` (new) |
| 5 | SubscriptionSyncActivity — blocking UI with per-series progress, skip button | `src/activities/network/SubscriptionSyncActivity.{h,cpp}` (new) |
| 6 | Boot-path hook in `main.cpp` | `src/main.cpp` |
| 7 | Watermark sidecar (`sub_watermark.bin`) + "— N new chapters —" break page | `src/activities/reader/EpubReaderActivity.{h,cpp}` |

### New files

- `src/network/SubscriptionState.{h,cpp}`
- `src/network/SubscriptionSyncer.{h,cpp}`
- `src/activities/network/SubscriptionSyncActivity.{h,cpp}`
- `docs/subscription-sync.md`

### Modified files

- `src/CrossPointSettings.h` — 3 new fields (`subscriptionsEnabled`, `subscriptionServerUrl[256]`, `subscriptionBearerToken[128]`)
- `src/SettingsList.h` — registered new settings under a `STR_SUBSCRIPTIONS` category; bearer token uses `.withObfuscated()`
- `src/network/HttpDownloader.{h,cpp}` — `HttpResult` struct + `fetchConditional()` + `downloadToFileConditional()`
- `src/main.cpp` — conditional `pushActivity(SubscriptionSyncActivity)` after the home/reader dispatch
- `src/activities/reader/EpubReaderActivity.{h,cpp}` — subscription detection + break page
- `lib/I18n/translations/english.yaml` — 17 new string keys

## Final footprint

- RAM: 30.0% (98,276 / 327,680 bytes) — +8 bytes BSS over baseline
- Flash: 89.1% (5,842,299 / 6,553,600 bytes) — +19 KB over baseline (~+22 KB counting the subscription-sync paths)

Build environment: `default` (LOG_LEVEL=2, serial enabled).

## Verified

- **Build**: `pio run` clean on `default` env after every phase.
- **Format**: clang-format applied (at `/opt/homebrew/opt/llvm@21/bin/clang-format` — not on PATH).

## Not verified (human tester)

- **On-device behavior**: configure settings → connect Wi-Fi once → sleep/wake → sync activity shows → EPUBs appear in `/.subscriptions/` on SD → reader opens them unchanged → break page triggers on first spine index past watermark.
- **304 fast-path timing**: target is ~1–2s end-to-end on subsequent wakes with no server changes.
- **Orphan cleanup**: server drops a series from `index.json` → device deletes `.epub` + its cache dir.
- **Heap headroom during sync**: TLS costs ~40–50 KB; confirm free heap stays above ~50 KB with a reader activity underneath.
- **Break page UX**: does not re-trigger within a session, does re-trigger after next sync, dismissed by forward press.

## Known costs / quirks

- **First open after sync re-lays out current chapter.** `Epub::load` wipes `sections/*.bin` on cache rebuild (at [lib/Epub/Epub.cpp:461](lib/Epub/Epub.cpp)) when CSS is being loaded. Lazy section generation absorbs the cost — a few seconds on the currently-read chapter, not the full book. Accepted.
- **Watermark semantics are "reset on exit"**: on `onExit`, the watermark is written to match the current spine count. So the break page shows once per sync — not once per unread chapter. Simple and predictable.
- **Wi-Fi lifecycle is activity-scoped**: the syncer brings Wi-Fi up and down itself; no global coordinator. If the user manually enters another network activity while sync is running, they'd race — but sync starts at boot before any user navigation, and `onExit` tears down cleanly, so collisions are unlikely in practice.
- **`WifiCredentialStore` is loaded on-demand**: `SubscriptionSyncActivity::onEnter` calls `WIFI_STORE.loadFromFile()` once. If the store grows large, this adds a small boot-time disk read.
- **No mid-download cancel**: Back button sets the abort flag, which is checked at phase boundaries and between chapters — but a single in-flight HTTP download runs to completion. For typical EPUBs on fast Wi-Fi this is fine; for huge backlogs on slow links, Skip may feel unresponsive for a few seconds.

## Explicitly deferred

- RTC timer-wake for unattended scheduled sync (v2).
- Library badge ("+N new" on covers) — polish after core flow verified on device.
- EPUB export-before-unsubscribe retention.
- Per-chapter delta sync.
- Multi-server / multi-account support.
- Mid-download abort (would need custom non-blocking socket usage).

## Related artifacts

- Plan: `/Users/dinanwu/.claude/plans/now-plan-out-the-zazzy-dream.md`
- Memory: `memory/project_web_serial_sync.md` (in Claude Code memory, not the repo)
- Server contract: [docs/subscription-sync.md](docs/subscription-sync.md)

## Next steps

1. **Server**: build the scraper + EPUB generator per the wire contract. Needs Royal Road chapter-ID-ordered append-only EPUB emission, `/v1/subs/index.json`, `/v1/subs/<id>.epub`, bearer auth, ETag on every response.
2. **On-device smoke test**: flash, configure, observe a real sync end-to-end.
3. **Heap profile during sync**: `LOG_INF("MEM", ...)` already emits every 10s; confirm no dangerous dips.
4. **Polish** (optional, after smoke test): library badge, settings web UI copy, error screens if common failure modes surface.
