# WireView protocol (version 1)

One TCP connection per session, default port 47110. Everything is little-endian and packed. The structs live in `src/protocol.h`.

## Handshake

1. The viewer connects and sends `Hello` (72 bytes): magic `WVW1`, version, flags (bit 0 = wants control), computer name.
2. The host answers `Welcome` (76 bytes): magic, version, status (0 ok, 1 rejected, 2 busy, 3 bad version), flags (bit 0 = control granted), computer name.

After that both sides exchange messages: an 8-byte `MsgHeader` (type, flags, payload size) followed by the payload.

## Host to viewer

| Type | Name | Payload |
|---|---|---|
| 1 | Mode | width, height, format (1 = BGRA8). Always followed by a full frame. |
| 2 | Frame | `FrameMsg` (seq, move count, rect count), `MoveRect[]`, `Rect[]`, then the pixels of each rect in order, rows of `width * 4` bytes, no padding |
| 3 | PointerPos | x, y (top-left of the pointer image), visible |
| 4 | PointerShape | DXGI pointer type, width, height, pitch, hotspot, then `pitch * height` bytes |
| 5 | Control | granted (0/1). Sent when the host changes "Allow remote control". |

A frame is applied in order: every move first (copy `dst`-sized area from `(srcX, srcY)` to `dst`, one after another), then every rect is overwritten with its pixels. Pixels are uncompressed BGRA straight from DXGI Desktop Duplication. Only the rectangles Windows reports as changed are sent; a static screen sends nothing.

## Viewer to host

| Type | Name | Payload |
|---|---|---|
| 16 | Input | `InputEvent[]`, 12 bytes each, at most 1024 |
| 17 | RequestFull | empty; the host replies with a full frame |

`InputEvent` is `kind, a, b, x, y`:

| kind | meaning |
|---|---|
| 1 | move to (x, y) in remote screen pixels |
| 2 | button `a` (0 left, 1 right, 2 middle, 3 X1, 4 X2), `b` 1 = down, at (x, y) |
| 3 | wheel, `a` 1 = horizontal, `x` = delta (120 per notch) |
| 4 | key, `b` = scan code, `a` bit 0 = up, bit 1 = extended |
| 5 | release everything held |

## Rules

The host checks every message before acting on it. An unknown type, a wrong size or an invalid event drops the viewer. Input is applied only while the host's "Allow remote control" is ticked, and that is checked on every message. When a session ends the host releases every key and button the viewer still held.

The viewer refuses frames whose rectangles leave the screen or whose size doesn't add up.

## Discovery

UDP port 47111. The viewer sends `WVW?` + version as a directed broadcast on each IPv4 adapter. A sharing host answers with `WVW!`, version, TCP port, flags (bit 0 = control allowed) and its name.
