# WireView

See and control another Windows PC in a window. Over a USB4 cable it sends the screen raw at full speed; over Wi-Fi or Ethernet it compresses.

Both PCs run the same program. One shares its screen, the other connects, sees it and, if allowed, uses it with its own mouse and keyboard. No account, no sign-in, no notifications, no installer. Free and open source (GPL-3.0).

## How it works

**On a USB4 cable.** Windows 11 creates a network adapter when two USB4 PCs are joined by a USB4 (or Thunderbolt 4) cable, and gives both ends a 169.254.x.x address. WireView runs over that link.

- **No compression.** Pixels go over the wire as raw 32-bit BGRA, exactly as Windows captured them.
- **Only what changed.** Windows' desktop capture (DXGI Desktop Duplication) reports which rectangles changed and which were moved (scrolling). WireView sends just those rectangles, and moves as 24-byte copy instructions. A still screen costs nothing.
- **No CPU copies on the sending side.** Changed rectangles are copied on the GPU into a readable texture, and the socket reads them straight from that memory.
- **Upload only changed rectangles on the receiving side,** draw with Direct3D 11, never queue frames.

Bandwidth for full-screen motion is roughly width x height x 4 x fps: a 1920x1080 screen at 60 fps is about 4 Gbit/s. A typical desktop is far less. USB4 links are 20 or 40 Gbit/s; that is the link rate, and what TCP actually gets over Windows' USB4 network adapter has not been measured yet.

**On anything else** (Wi-Fi, Ethernet, a phone hotspot) raw pixels are too much, so by default the picture is scaled to 720p on the GPU and each changed area is compressed, the way VNC's Tight encoding does it:

- areas with up to 256 colours (text, windows, UI) are sent **lossless**, as a colour table plus compressed indices, so text stays sharp. A line of text typically shrinks 50-100x.
- everything else (photos, video) is sent as JPEG.
- large areas are split into 256x256 tiles, so a window with text and a picture gets both treatments.

Both use what is built into Windows (WIC and the Compression API). On a Surface Laptop 3 a full 720p frame encodes in about 8 ms and is around 90 KB; full-screen video at 30 fps is about 20 Mbit/s, and normal desktop work much less.

**Control** is a small side channel: 12-byte mouse and keyboard events, replayed with `SendInput` on the shared PC.

## Picture settings

**View > Picture** (or the Connect dialog):

| Setting | Use |
|---|---|
| Automatic (default) | raw on a USB4 / Thunderbolt link, compressed 720p on anything else |
| Raw, full resolution | USB4 |
| Compressed, full resolution | fast Ethernet, when you want every pixel |
| Compressed, 1080p | Ethernet or good Wi-Fi |
| Compressed, 720p | Wi-Fi, hotspots, slow links |

It can be changed while connected. The status bar shows the stream size, frame rate and bit rate.

## Requirements

- Windows 10 or 11, 64-bit, on both PCs, both running the same WireView version.
- For the cable: USB4 or Thunderbolt 4 ports on **both** PCs, Windows 11, and a USB4 / Thunderbolt cable. A plain USB-C cable between two ordinary USB-C ports does not create a link: both ports are hosts, and no software can change that.
- Otherwise: both PCs on the same network. Many school and office Wi-Fi networks block devices from reaching each other; a phone hotspot or an Ethernet cable avoids that.

## Use

On the PC to be shared:

1. **Share > Share this screen.**
2. To let the other PC control it, also tick **Share > Allow remote control**.
3. The first time, Windows Firewall asks about network access. A USB4 link counts as an "unidentified network", which Windows treats as **Public**, so allow Public networks. Or use **Share > Allow through firewall...** (needs admin), which allows WireView on all network types.

On the other PC:

1. **File > Connect...** It lists PCs that are sharing, USB4 links first. Double-click one, or type an address.
2. Keep **Ask to control the other PC** ticked if you want control.
3. The shared PC asks whether to let you in.
4. **Control > Take control** (Ctrl+Alt+F12) sends your mouse and keyboard to the other PC, including Alt+Tab and the Windows key. Press **Ctrl+Alt+F12** again to get them back.

Other options: **View > Full screen** (F11), **View > Refresh** (F5) to resend the whole screen, **Share > Monitor** to pick which screen to share. Settings are kept in `HKEY_CURRENT_USER\Software\WireView`.

Command line: `WireView.exe --share` starts sharing, `WireView.exe --connect 169.254.10.20` connects.

## Limits

- Not tested on USB4 hardware or between two PCs yet. It was developed and tested on one PC over loopback.
- Not encrypted. It is meant for a cable or a network you trust. Without "Ask before accepting", anyone who can reach the port can see the screen.
- Windows does not let a normal program see or click the secure desktop: UAC prompts, the lock screen, Ctrl+Alt+Del. The viewer shows the last picture until the desktop comes back. Apps running as administrator on the shared PC can only be controlled if WireView there also runs as administrator.
- Rotated monitors and HDR are not supported yet.
- One viewer at a time.

## Build

Visual Studio 2022 or later with the C++ desktop workload, and CMake 3.20 or later:

```
cmake -S . -B build
cmake --build build --config Release
build\Release\wireview_tests.exe
```

`wireview_tests.exe` checks the protocol parsers, the codecs, input mapping, and GPU frame updates against a CPU reference. It also runs real capture sessions over loopback (raw and compressed 720p); input there goes to a recorder, never to the real desktop. `wireview_tests.exe --bench` measures loopback throughput, capture speed and encode speed.

There are no dependencies beyond the Windows SDK. The C runtime is linked statically, so `WireView.exe` is a single file.

## Layout

```
src/protocol.*   wire format and strict parsing
src/net.*        sockets, adapters
src/discovery.*  finding sharing PCs (UDP broadcast)
src/capture.*    DXGI Desktop Duplication, GPU downscaling
src/codec.*      lossless palette and JPEG encodings for slow links
src/host.*       the sharing side: capture, encode, send, receive input
src/inject.*     input events -> SendInput
src/viewer.*     the viewing side: receive, decode, send input
src/render.*     Direct3D 11 drawing
src/cursor.*     remote pointer shapes
src/main.cpp     the window, menus and dialogs
docs/protocol.md the protocol
```

## License

GPL-3.0. See [LICENSE](LICENSE).
