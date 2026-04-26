# fmp4-packager

On-the-fly HLS fMP4 packager that reads MKV files over HTTP byte-range and
emits CMAF-style fragmented MP4 segments. Replaces the FFmpeg mp4 muxer with
a custom fMP4 box writer (inspired by [nginx-vod-module](https://github.com/kaltura/nginx-vod-module)),
which removes the post-hoc `tfdt`/`sidx`/`mfhd` patching and muxer-state
juggling that the older `hlsenc-fork` implementation required.

## What it does

```
WebTorrent (byte-range HTTP)
        │ libcurl
        ▼
   HttpReader → AVIOContext
        │
        ▼
   libavformat (MKV demux)
        │
   ┌────┴────────────────────────────┐
   │ video AVPacket  audio AVPacket  │
   │   (passthrough)        │        │
   │                        ▼        │
   │              libavcodec decoder │
   │                        │        │
   │                  swresample     │
   │                        │        │
   │              libavcodec AAC enc │
   └────┬────────────────────┬───────┘
        ▼                    ▼
            Fmp4Writer
                │
     init.mp4 / N.m4s bytes
                │
                ▼
        httplib server (:8084)
```

## Endpoints

- `GET /health` — liveness probe
- `GET /:infohash.m3u8` — creates a session, returns the HLS VOD manifest
- `GET /:infohash/session-:id/init.mp4` — fMP4 initialization segment
- `GET /:infohash/session-:id/:n.m4s` — fMP4 media segment

The `:infohash` is the BitTorrent infohash, looked up against a WebTorrent
HTTP file server at `WEBTORRENT_URL` (default `http://localhost:8083`).

## Build

```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j
```

Run:

```bash
./build/fmp4-packager        # PORT=8084 by default
WEBTORRENT_URL=http://localhost:8083 PORT=8084 ./build/fmp4-packager
```

## Environment variables

- `PORT` — listen port (default `8084`)
- `WEBTORRENT_URL` — base URL of the WebTorrent file server (default `http://localhost:8083`). Files are fetched from `${WEBTORRENT_URL}/files/${infohash}/${file_index}`
- `FFMPEG_LOG_LEVEL` — numeric `av_log_set_level` value (default `AV_LOG_FATAL` = 8). Set to `32` (`AV_LOG_INFO`) to see codec-level warnings such as TrueHD substream length mismatches.

## Architecture

See `[plan]` for the full design rationale. Key invariants:

1. **`Fmp4Writer::BuildSegment` is a pure function.** Every segment's bytes
   depend only on `(sequence_number, tracks, samples, tfdt)`. There is no
   cross-segment muxer state. Re-fetching segment N always returns identical
   bytes (critical for hls.js retry/seek).

2. **Seek and sequential code paths are identical.** No muxer to tear down,
   no `delay_moov` dual-flush, no first-segment timing capture.

3. **`tfdt` is computed from segment start time alone.** No tracking of
   `last_video_tfdt_end_` etc.

## Drop-in replacement for `hlsenc-fork`

`fmp4-packager` is wire-compatible with `hlsenc-fork` on port 8084: same routes,
same URL structure, same response bodies. Swap procedure:

```bash
# Stop the old packager (if it's running):
killall hlsenc-server

# Start the new one (default PORT=8084):
WEBTORRENT_URL=http://127.0.0.1:8083 ./build/fmp4-packager
```

`diode-core/backend` proxies via `SEGMENT_API` (defaults to
`http://127.0.0.1:8084`) and does not need any code changes.

## Limitations

- MKV input only (libavformat will accept MP4 too, but untested)
- Single bitrate, no ABR
- VOD only, no live
- No DRM / encryption
