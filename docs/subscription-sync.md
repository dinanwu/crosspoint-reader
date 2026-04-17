# Subscription Sync — Server Contract

**Status**: Draft design. Not yet implemented.

Defines the wire contract between the device (CrossPoint firmware) and an external server that scrapes web serials (Royal Road, etc.) and publishes them as EPUBs. The device polls the server on wake and downloads updated EPUBs so new chapters appear without manual sideloading.

Subscription management (adding/removing series) is handled entirely by the server's web UI and is out of scope for this document.

---

## Architecture at a glance

- Server runs a scheduled scraper, builds one EPUB per subscribed series, and exposes them over HTTPS.
- Device syncs on wake (if interval elapsed) via a blocking `SubscriptionSyncActivity`. Activity downloads any changed EPUBs and hands off to home/reader.
- Device treats subscription EPUBs like any other EPUB — no new reader code, no new format on disk.
- A small sidecar (`sub_watermark.bin`) stored beside progress.bin tracks "new since last open" for the break-page UX.

---

## Design invariants

These rules are what keep device-side reading progress valid across sync updates. The server **must** honor them.

1. **Chapter order is stable across rebuilds.** Sort spine items by source chapter ID ascending (e.g. Royal Road chapter ID).
2. **Append-only.** New chapters added at the end. Never insert into the middle, never reorder existing items.
3. **One source-chapter = one spine item.** Don't combine multiple chapters into a single XHTML file; don't split a single chapter across multiple files.
4. **TOC order matches spine order.** No hierarchy, no rearrangement.
5. **Spine index N today = spine index N tomorrow**, as long as chapter N still exists on the source. This is the invariant `progress.bin` depends on.

For deleted source chapters (rare on RR, but possible): leave a placeholder spine item with a deprecation note ("chapter removed by author"). Never collapse indices. (v2 concern; not a v1 blocker.)

---

## Endpoints

Both endpoints are under a `/v1/` prefix for forward compatibility.

```
GET /v1/subs/index.json
GET /v1/subs/<series-id>.epub
```

Series IDs must be source-prefixed (`royalroad-107917`, `scribblehub-98765`). This avoids collisions across sources and makes device-side filenames self-documenting.

---

## Authentication

Single static bearer token, configured on both ends. No rotation mechanism.

```
Authorization: Bearer <token>
```

Stored device-side in settings. Not a user/password pair — do not reuse the existing Basic-auth OPDS credential fields. Firmware sends via `HTTPClient::addHeader`.

---

## TLS

HTTPS required. Device uses `NetworkClientSecure::setInsecure()` ([src/network/HttpDownloader.cpp](../src/network/HttpDownloader.cpp)) — it will complete any valid TLS handshake but does not verify certificates. The bearer token is the real security boundary.

Server needs valid TLS for TLS to complete; any cert works. Let's Encrypt is fine.

---

## Index format

`GET /v1/subs/index.json` returns:

```json
{
  "formatVersion": 1,
  "generatedAt": 1713024000,
  "series": [
    {
      "id": "royalroad-107917",
      "title": "Sky Pride",
      "author": "Gravity Tales",
      "url": "/v1/subs/royalroad-107917.epub",
      "etag": "W/\"abc123\"",
      "size": 2457600,
      "updatedAt": 1712956800,
      "chapterCount": 42
    }
  ]
}
```

### Fields

| Field | Type | Purpose |
|---|---|---|
| `formatVersion` | int | Always `1` for this version of the contract. Device aborts on mismatch. |
| `generatedAt` | unix seconds | When the server built this index. Informational. |
| `series[]` | array | Active subscriptions. Empty array is valid and triggers no action (see "Unsubscribe"). |
| `series[].id` | string | Source-prefixed stable ID. Used as filename on device. |
| `series[].title` | string | Display title. |
| `series[].author` | string | Display author. |
| `series[].url` | string | Relative path to the EPUB (device prefixes with server base). |
| `series[].etag` | string | ETag of the EPUB resource. Lets device skip the conditional GET entirely if unchanged since last sync. |
| `series[].size` | int (bytes) | For download progress bar. |
| `series[].updatedAt` | unix seconds | Last time the EPUB was rebuilt. Used for "updated X ago" display. |
| `series[].chapterCount` | int | Current chapter count. Lets device show "N new chapters" without parsing the EPUB. |

The index endpoint itself must support conditional GET (see below). Device skips all per-series work on a `304`.

---

## EPUB generation rules

### Structure

- **EPUB 3** with `nav.xhtml`. Device parses both 2 and 3 ([lib/Epub/Epub.cpp:410-419](../lib/Epub/Epub.cpp)), but 3 is cleaner.
- **Cover image** in manifest. Prefer dimensions close to 800×480 grayscale — the device converts to BMP on load, and smaller-to-decode is cheaper.
- **Metadata**: `dc:title`, `dc:creator`, `dc:language`. The device's EPUB parser expects standard OPF.

### Content

**XHTML tag whitelist** (anything else: strip):

- Block: `<h1>`, `<h2>`, `<p>`, `<blockquote>`, `<hr>`, `<br>`
- Inline: `<em>`, `<strong>`, `<b>`, `<i>`, `<u>`, `<a href>`
- Media: `<img src>` (LitRPG status screenshots)
- Tables: `<table>`, `<tr>`, `<td>` (LitRPG stat blocks)

**Must strip**: scripts, `<style>`, `class`/`id` attributes (unless used for fragment navigation), Royal Road chrome, ad containers, social share widgets, comment sections, nav divs, spoiler-tag UI.

**CSS**: none, or minimal. The reader uses user-configured font/size/spacing; embedded CSS fights that. Bonus: zero CSS means no section-cache wipe on reload (see [Epub.cpp:461](../lib/Epub/Epub.cpp) — `parseCssFiles()` triggers `removeDir(sections)` when CSS is present). Zero CSS is the cheapest choice all around.

**Author notes**: make configurable server-side, default to "keep." Easier to strip them later than to rescrape for content you dropped.

---

## Caching / conditional GETs

Both endpoints must set an `ETag` header on every response. Device sends `If-None-Match: <stored-etag>` on every request. Server returns `304 Not Modified` when the ETag matches, `200 OK` with a new ETag when content changed.

- Weak ETags (`W/"..."`) are acceptable; byte-exact semantics aren't needed.
- For the EPUB endpoint, a content hash of the file bytes is the natural ETag source.
- For the index, a hash of the serialized JSON.
- `Last-Modified` / `If-Modified-Since` optional but useful for debugging.
- `Cache-Control: private, max-age=0` recommended.

### Two-tier cache flow

1. Device sends `If-None-Match: <indexEtag>` to `/v1/subs/index.json`.
2. On `304`: sync completes in ~1s, no further requests.
3. On `200`: device parses the new index, compares each `series[].etag` against locally stored per-series ETags.
4. For each changed series: `GET /v1/subs/<id>.epub` with conditional `If-None-Match`. Download if server returns `200`.
5. Device persists the new index ETag and per-series ETags after successful application.

---

## Error handling

Device behavior per HTTP status:

| Status | Behavior |
|---|---|
| `200 OK` | Use response body. |
| `304 Not Modified` | Skip this resource, continue. |
| `401 Unauthorized` | Terminal for this sync. Bad or missing bearer. Log, surface "sync failed", do not retry until user reconfigures. |
| `404 Not Found` | Skip this series, continue with the rest. Do not abort the whole sync. |
| `5xx` | Transient. Abort this sync, retry on next boot. |

Malformed JSON, truncated downloads, or unexpected payloads → abort sync, log, retry next boot.

---

## Unsubscribe

When a series disappears from `index.json`:

- Device removes the local `.epub` at `/.subscriptions/<series-id>.epub`.
- Device nukes the associated cache directory at `/.crosspoint/epub_<hash>/` (including watermark sidecar).

This matches the "server is source of truth for subscriptions" philosophy. Users who want to keep a downloaded serial after unsubscribing should copy the EPUB out of `/.subscriptions/` before the next sync.

### Guardrail

Only honor deletions if the top-level response is well-formed (`formatVersion == 1`, valid JSON). A 5xx or parse failure leaves local state untouched. This avoids a server bug wiping the reader's library.

---

## Device-side sync state

Per-series state (inside the book's existing cache directory):

- `sub_watermark.bin` in `.crosspoint/epub_<hash>/` — `uint16_t lastOpenedSpineCount` for the break-page feature. Its presence also marks "this EPUB is a subscription."

Global sync state:

- `indexEtag` — ETag of the last successfully applied index.json. Persisted in SPIFFS settings.
- `seriesEtags` — small JSON map of `{id: etag}` for per-series conditional GETs. Written atomically to `/.subscriptions/state.json` after every successful sync.
- `lastSyncTick` — `RTC_DATA_ATTR uint64_t`. Survives deep sleep. Used for the "interval elapsed?" check at boot. Resets on full power loss (acceptable — next boot triggers a sync).

---

## Versioning policy

- URL prefix (`/v1/`) and JSON `formatVersion` must both match what the device supports.
- Breaking changes → new URL prefix (`/v2/`), new `formatVersion`. Device firmware update required to use the new version.
- Additive changes (new optional fields) may be deployed under `/v1/` without breaking older firmware.

---

## Open items

Things to decide when building the server:

- **EPUB generation library**: Python `ebooklib` is the obvious default; hand-rolled ZIP with OPF/nav.xhtml templates is also viable.
- **Cover image dimensions**: recommend downsizing to ~400×600 grayscale before embedding. Verify device decode cost.
- **Scrape scheduling**: server-side cron interval. Independent of device sync interval. Probably 30 min – 2 hr.
- **Author note handling**: default "keep"; make server-side config.
- **Per-chapter revision handling**: if an RR author edits a published chapter, does the server re-emit the EPUB with that chapter updated in place? Current invariants allow this (content of spine item N changes, index stays N). Confirm scraper behavior matches.
