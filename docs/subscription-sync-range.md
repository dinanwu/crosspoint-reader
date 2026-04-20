# Subscription Sync — Range Download Extension (Server Contract)

**Status**: Proposed. Not yet implemented on device or server.

**Audience**: Server engineer implementing the CrossPoint subscription EPUB endpoint.

**Relationship to [subscription-sync.md](./subscription-sync.md)**: additive. This document defines new optional fields and new build constraints that unlock a device-side optimization. The base wire contract in `subscription-sync.md` continues to apply; `formatVersion` stays at `1`. When this extension ships and is proven, the contents of this doc should fold into `subscription-sync.md` and this file can be retired.

---

## Why

Today the device re-downloads the entire EPUB whenever a series is updated. A 2 MB serial takes ~18 s over Wi-Fi + HTTPS + SD-write on the ESP32-C3 ([subscription-sync-internals.md log at ~506](./subscription-sync-internals.md)). A typical update adds a single ~30 KB chapter. The wire cost is ~80× the information delta.

Because EPUBs are ZIPs and the existing design invariants already guarantee append-only spines with stable indices, the server can lay out ZIP entries so that all newly-appended content lives at the **tail** of the file. The device can then `Range: bytes=X-` just the tail. No changes to the reader, no changes to the EPUB format on disk.

Expected result: ~1.5 s per updated series, roughly 10× faster than today.

---

## Summary of required server changes

| Change | Where | Effort |
|---|---|---|
| Lock ZIP entry order so chapter bytes form a stable prefix | EPUB build pipeline | Small — a 5-line ordering change if using `zipfile.writestr` |
| Make chapter compression byte-deterministic across rebuilds | EPUB build pipeline | Small — pin zlib level, fix timestamps |
| Compute and publish `stablePrefixLength` per series | EPUB build pipeline + index builder | Small — one `fp.tell()` call during build |
| Compute and publish `contentHash` per series | EPUB build pipeline + index builder | Small — hash the finished file |
| Serve HTTP `Range` requests on the EPUB endpoint | Static file server config | Zero for nginx/Caddy; trivial for anything else |
| Use strong `ETag`s on EPUB responses | Static file server config | Usually default; verify |
| Add a build-time byte-stability test | CI | Small — one script, runs on every build |

Device firmware changes are tracked separately. This document is scoped to the server-side contract only.

---

## Index format changes

Two new fields are added to each entry in `series[]` of `GET /v1/subs/index.json`. Both are REQUIRED when this extension is active; device firmware that supports the extension treats their absence as a signal to fall back to full-file downloads for that series.

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
      "etag": "\"abc123\"",
      "size": 2457600,
      "updatedAt": 1712956800,
      "chapterCount": 42,

      "stablePrefixLength": 2340192,
      "contentHash": "sha256:9c1185a5c5e9fc54612808977ee8f548b2258d31"
    }
  ]
}
```

### `stablePrefixLength` (int, bytes) — REQUIRED

The byte offset in this EPUB at which the stable region ends and the regenerated region begins. Equivalently: the offset of the local file header of the first entry that was rewritten in this build (i.e., the OPF).

The server MUST guarantee that for any two builds B₁ and B₂ of the same series where:
- `chapterCount(B₂) >= chapterCount(B₁)`, and
- no chapter present in B₁ has had its source content edited between the builds,

the first `stablePrefixLength(B₁)` bytes of B₂ are bit-identical to the first `stablePrefixLength(B₁)` bytes of B₁.

This is what lets the device keep its existing local bytes and request only `bytes=<oldStablePrefixLength>-` on the next sync.

### `contentHash` (string) — REQUIRED

SHA-256 of the full EPUB file bytes, lowercase hex, with a `sha256:` prefix. Example: `"sha256:9c1185a5c5e9fc54612808977ee8f548b2258d319c1185a5c5e9fc54612808977ee8f548"`.

The prefix is reserved for future algorithm changes; today only `sha256` is valid. Device firmware MUST reject entries with any other prefix.

The device computes the same hash over the locally-assembled file after a range update and compares. On mismatch it discards the local file and falls back to a full re-download. This is the only defense against silent corruption caused by a server-side build regression that breaks the stability invariant.

### Unchanged fields

`etag`, `size`, `updatedAt`, `chapterCount`: same semantics as [subscription-sync.md](./subscription-sync.md). `etag` remains the ETag of the full file (same as the `ETag` header returned on the EPUB endpoint).

---

## EPUB build requirements

These three requirements replace nothing; they layer on top of the existing invariants in [subscription-sync.md](./subscription-sync.md) ("Design invariants" and "EPUB generation rules").

### Invariant 6: ZIP entry ordering

Entries MUST be written to the ZIP in this order:

1. `mimetype` (uncompressed, `ZIP_STORED`, as the EPUB spec already requires)
2. `META-INF/container.xml`
3. Cover image (if present)
4. Chapter XHTML files, in spine order (which equals source-chapter-ID ascending order per existing invariant 1)
5. `content.opf` (or whatever path `container.xml` points to)
6. `nav.xhtml`
7. (central directory written automatically by the ZIP writer)

This ordering puts everything that can change on a chapter-addition (OPF spine entry, nav TOC entry, central directory) strictly after everything that cannot (chapter bytes).

If the server embeds any other per-build-variable content (e.g., a "last-updated" XHTML page, injected CSS whose content depends on build time), it MUST live in the OPF/nav region at the tail, after the last chapter. Static assets referenced by chapters (fonts, shared images if any) belong in the stable prefix; they must be deterministic.

### Invariant 7: Deterministic compression

Two rebuilds of the same chapter source content MUST produce byte-identical compressed entries in the stable region. This means, at minimum:

- **Pin the compression level.** Python's `zipfile` defaults to `Z_DEFAULT_COMPRESSION` (level -1 → 6). Explicit is safer: pass `compresslevel=6` to `ZipFile` (Python 3.7+) or use `ZIP_DEFLATED` with a pinned zlib version.
- **Fix local file header timestamps.** `zipfile.writestr(name, data)` defaults to "now" for the DOS mtime embedded in each local file header. Across rebuilds, this introduces 4 bytes of drift per entry, which:
  - Does NOT shift chapter byte offsets (LFH size is fixed).
  - Does NOT cause reader-side functional errors (central directory CRCs are over uncompressed data; LFH timestamps are read but not validated against anything).
  - DOES mean the hash check in section 5 would fail on the device after a range update, because the locally-stored chapter LFH bytes (from an earlier build) would not match what the server's current full-file hash covers.

  Pass an explicit `ZipInfo` with a fixed `date_time` (e.g., `(1980, 1, 1, 0, 0, 0)`) for every entry, or honour `SOURCE_DATE_EPOCH`. Both are standard conventions for reproducible builds.
- **No `zip64` extensions in the stable region.** EPUBs never exceed 4 GB; `zipfile` will not promote automatically unless an entry is huge. Nothing to do here; just do not force `zip64`.
- **No data descriptors.** `zipfile.writestr` writes sizes into the local file header directly when writing from an in-memory bytes object, which is what we want. Streaming writes use trailing data descriptors; avoid that mode.

The combined effect: every chapter occupies bit-identical bytes across rebuilds, and `stablePrefixLength` grows monotonically as chapters are appended.

### Invariant 8: Frozen chapter content

Once a chapter has been published in any build, its source XHTML bytes MUST NOT change in any later build. If a scrape updates a chapter's content, the server MUST either:

- (a) ignore the update and keep the original bytes, or
- (b) publish a new build with a fresh `stablePrefixLength` that starts at or before the edited chapter's old offset, forcing the device to fall back to a full download (the server returns `200 OK` rather than `206 Partial Content`; see section 4).

The product decision per [internal conversation linked to this proposal] is (a): author-edited chapters are explicitly out of scope. If the scraper detects an edit, log and drop it.

---

## HTTP endpoint requirements

### Range support

`GET /v1/subs/<series-id>.epub` MUST honour `Range` requests. Response rules:

| Request headers | Response |
|---|---|
| `If-None-Match: <current-etag>` | `304 Not Modified`, no body. Evaluate this *before* `Range` per RFC 7232 §6. |
| `Range: bytes=X-` where `X < size` | `206 Partial Content` with body bytes `X..size-1`, `Content-Range: bytes X-(size-1)/size`, `Content-Length: size-X`, `ETag: <current-etag>`. |
| `Range: bytes=X-` where `X >= size` or request is otherwise unsatisfiable | `416 Range Not Satisfiable`, OR fall back to `200 OK` with full body. Either is acceptable; the device treats both as "range failed, try again without Range next time." |
| No `Range`, no matching `If-None-Match` | `200 OK` with full body and `ETag` (current behaviour). |

The device will typically send `If-None-Match` and `Range` together. `If-None-Match` wins → `304` first; otherwise `206`.

### Strong ETags

`ETag` on the EPUB endpoint MUST be a strong validator (no `W/` prefix). RFC 7233 §3.2 allows servers to refuse `Range` when only a weak validator matches.

A content hash of the full file bytes (SHA-256 truncated to 16 hex chars works fine) is a valid strong ETag. If the server is nginx/Caddy serving the EPUB as a static file, the default inode+mtime-based ETag is strong and also fine.

### Configuration notes for common servers

- **nginx**: `Range` support is on by default for static files. Ensure `etag on;` (default). No changes needed unless a proxy in front strips `Range`.
- **Caddy v2**: `Range` and strong `ETag` are on by default for the `file_server` directive. No changes needed.
- **Custom Python (Flask/FastAPI)**: you need to implement `Range` and `Content-Range` yourself, or put nginx in front. Starlette's `FileResponse` handles Range correctly; Flask's `send_file` does not in older versions — verify on your version.
- **CDNs (CloudFlare, Fastly)**: most honour `Range` transparently, but some strip it on certain cache configurations. Verify with `curl -H 'Range: bytes=100-200' -v <url>` against the production endpoint before shipping.

---

## Backward compatibility

### Old firmware + new server

Old firmware ignores `stablePrefixLength` and `contentHash` fields. Full-file download path continues to work unchanged. No-op.

### New firmware + old server (fields missing)

Device detects absence of `stablePrefixLength` in the index entry and falls back to full-file download for that series. No-op.

### New firmware + new server, first sync

Device has no local `.epub` for this series yet. Nothing to range-patch onto; device does a full download (`200 OK`), then stores `stablePrefixLength` and `contentHash` for next time.

### New firmware + new server, series removed then re-added

Device deleted the `.epub` during orphan cleanup. On re-add it has no local file; treated as first sync. No-op.

### New firmware + new server, build regression violates stability

Device range-downloads, assembles the file, computes `contentHash`, mismatches. Device discards local file, forces full download on next sync. One sync cycle wasted; no corruption reaches the reader.

---

## Testing requirements

### Byte-stability regression test (required, every CI run)

Script that does the following:

1. Pick a representative series (e.g., a mock with 40 chapters of ~30 KB each).
2. Build the EPUB → `B1`. Record `stablePrefixLength(B1)`.
3. Append one new chapter to the source.
4. Build the EPUB again → `B2`. Record `stablePrefixLength(B2)`.
5. Assert: `B1[0 : stablePrefixLength(B1)] == B2[0 : stablePrefixLength(B1)]` byte-for-byte.
6. Assert: `stablePrefixLength(B2) > stablePrefixLength(B1)`.
7. Assert: `sha256(B2)` matches the server's published `contentHash` for B2.

This test catches every realistic server-side regression: reordering entries, non-deterministic compression, timestamp leakage, `contentHash` drift.

### Manual probe of the deployed endpoint

Before declaring rollout complete, from a workstation:

```bash
# Full download baseline
curl -s -o full.epub -H 'Authorization: Bearer <token>' \
     https://.../v1/subs/royalroad-107917.epub
sha256sum full.epub   # Must match contentHash in index.json

# Range download of the tail
STABLE=2340192   # value from index.json
curl -s -H 'Authorization: Bearer <token>' \
     -H "Range: bytes=${STABLE}-" \
     -D headers.txt -o tail.bin \
     https://.../v1/subs/royalroad-107917.epub
grep -i '^HTTP\|^Content-Range\|^ETag' headers.txt
# Must see: 206 Partial Content, Content-Range: bytes 2340192-..., ETag matching full.epub's

# Reassemble and verify
head -c ${STABLE} full.epub > reassembled.epub
cat tail.bin >> reassembled.epub
diff <(sha256sum reassembled.epub) <(sha256sum full.epub)   # Must be identical
```

### Range-under-proxy check

If anything sits between the origin and the device (CDN, load balancer, cache proxy), repeat the range probe through it. Proxies are the single most common reason `Range` stops working in production.

---

## Reference build snippet (Python)

Illustrative only — adapt to your actual build pipeline.

```python
import hashlib, os, zipfile
from io import BytesIO

# Reproducible timestamp for every LFH.
FIXED_DATE = (1980, 1, 1, 0, 0, 0)

def add(zf, path, data, stored=False):
    info = zipfile.ZipInfo(path, date_time=FIXED_DATE)
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    # ZipInfo honours the compresslevel passed to writestr in 3.7+.
    zf.writestr(info, data, compresslevel=6)

def build(series_id, chapters_sorted_by_id, opf_bytes_fn, nav_bytes_fn,
          container_xml, cover_bytes, out_path):
    buf = BytesIO()
    with zipfile.ZipFile(buf, 'w') as zf:
        add(zf, 'mimetype', b'application/epub+zip', stored=True)
        add(zf, 'META-INF/container.xml', container_xml)
        if cover_bytes:
            add(zf, 'OEBPS/cover.jpg', cover_bytes)

        for ch in chapters_sorted_by_id:
            add(zf, f'OEBPS/{ch.filename}', ch.xhtml_bytes)

        # Record the offset of the first non-stable entry BEFORE writing it.
        stable_prefix_length = buf.tell()

        add(zf, 'OEBPS/content.opf', opf_bytes_fn(chapters_sorted_by_id))
        add(zf, 'OEBPS/nav.xhtml',   nav_bytes_fn(chapters_sorted_by_id))
        # central directory is written by ZipFile.__exit__

    blob = buf.getvalue()
    with open(out_path, 'wb') as f:
        f.write(blob)

    return {
        'stablePrefixLength': stable_prefix_length,
        'contentHash': 'sha256:' + hashlib.sha256(blob).hexdigest(),
        'size': len(blob),
    }
```

Two things to notice:

- `buf.tell()` is captured between the last chapter and the first changeable entry. This is the exact value the device needs.
- Chapters are added in `chapters_sorted_by_id` order. Preserving this invariant across refactors is the single most important thing to lock down in review.

---

## Rollout sequence

1. **Ship server changes only.** Fields present in the index, entry ordering locked, Range and strong ETag verified. Device firmware still does full downloads — no user-visible change.
2. **Let it bake for a week or two.** Any accidental build non-determinism shows up as `contentHash` instability across rebuilds with the same input. Monitor by hashing the build output on each CI run and alerting on unexpected churn.
3. **Ship device firmware that uses `stablePrefixLength` + Range.** Device firmware tracks separately; rolling it out early has no effect unless the server fields are populated.
4. **Fold this document into `subscription-sync.md`** and delete this file.

---

## Open questions / decisions deferred

- **Optional `Last-Modified` on EPUB responses.** Not required; ETag-based caching is sufficient. But nginx/Caddy emit it by default and it aids debugging.
- **Multi-range requests.** Device never sends them. Server may treat them as "not supported" and return `200 OK` with the full body.
