# WireView protocol (version 2)

One TCP connection per session, default port 47110. Everything is little-endian and packed. The structs live in `src/protocol.h`.

## Handshake

1. The viewer connects and sends `Hello` (80 bytes): magic `WVW1`, version, flags (bit 0 = wants control), computer name, then the stream settings `maxHeight` and `quality` (see below).
2. The host answers `Welcome` (76 bytes): magic, version, status (0 ok, 1 rejected, 2 busy, 3 bad version), flags (bit 0 = control granted), computer name.

The host reads magic and version first, so a viewer of any other version gets status 3 straight away, whatever the size of its Hello.

After that both sides exchange messages: an 8-byte `MsgHeader` (type, flags, payload size) followed by the payload.

## Stream settings

`maxHeight`: 0 = native resolution, otherwise the screen is scaled down on the GPU to that height (240 or more, never up). `quality`: 0 = raw pixels, 1-100 = compressed (JPEG quality for photo-like areas).

They are sent in the Hello (so the very first frame already has the right size) and can be changed during the session with a StreamSettings message.

## Host to viewer

| Type | Name | Payload |
|---|---|---|
| 1 | Mode | stream width, height, format (1 = raw BGRA, 2 = encoded), native width, native height. Always followed by a full frame. |
| 2 | Frame | `FrameMsg` (seq, move count, rect count), `MoveRect[]`, `Rect[]`, then the pixels of each rect in order (see below) |
| 3 | PointerPos | x, y (top-left of the pointer image, native pixels), visible |
| 4 | PointerShape | DXGI pointer type, width, height, pitch, hotspot, then `pitch * height` bytes (native pixels) |
| 5 | Control | granted (0/1). Sent when the host changes "Allow remote control". |

Rects and moves are in stream pixels. Pointer positions and input coordinates are always in native pixels.

A frame is applied in order: every move first (copy a `dst`-sized area from `(srcX, srcY)` to `dst`, one after another), then every rect is overwritten with its pixels. Only the rectangles Windows reports as changed are sent; a still screen sends nothing. Scaled streams have no moves.

Pixels per rect:

- format 1 (raw): rows of `width * 4` bytes, BGRA, no padding.
- format 2 (encoded): an 8-byte `RectData` (encoding, 3 reserved bytes, size) and `size` bytes:
  - 0 raw: rows of BGRA, `size` = width * height * 4
  - 1 JPEG: a baseline JPEG of exactly the rect's size
  - 2 palette: u16 colour count (1-256), u16 0, count x u32 colour (B, G, R, 0), then one byte per pixel (colour index, row by row) compressed with the Windows Compression API, XPRESS-Huffman, block mode

  An encoded rect is never larger than its raw size; the host sends it raw instead.

## Viewer to host

| Type | Name | Payload |
|---|---|---|
| 16 | Input | `InputEvent[]`, 12 bytes each, at most 1024 |
| 17 | RequestFull | empty; the host replies with a full frame |
| 18 | StreamSettings | maxHeight, quality |

`InputEvent` is `kind, a, b, x, y`:

| kind | meaning |
|---|---|
| 1 | move to (x, y) in native screen pixels |
| 2 | button `a` (0 left, 1 right, 2 middle, 3 X1, 4 X2), `b` 1 = down, at (x, y) |
| 3 | wheel, `a` 1 = horizontal, `x` = delta (120 per notch) |
| 4 | key, `b` = scan code, `a` bit 0 = up, bit 1 = extended |
| 5 | release everything held |

## Rules

The host checks every message before acting on it. An unknown type, a wrong size, an invalid event or invalid settings drops the viewer. Input is applied only while the host's "Allow remote control" is ticked, and that is checked on every message. When a session ends the host releases every key and button the viewer still held.

The viewer refuses frames whose rectangles leave the screen, whose sizes don't add up, or whose encoded rects don't decode to exactly the rect's size.

## Discovery

UDP port 47111. The viewer sends `WVW?` + version as a directed broadcast on each IPv4 adapter. A sharing host answers with `WVW!`, version, TCP port, flags (bit 0 = control allowed) and its name. Only hosts of the same version are listed.
