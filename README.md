# WireView

See and control another Windows PC in a window, over a USB4 cable between the two PCs.

Both PCs run the same program. One shares its screen, the other connects, sees it and, if allowed, uses it with its own mouse and keyboard. Free and open source (GPL-3.0).

## How it works

Windows 11 creates a network adapter when two USB4 PCs are joined by a USB4 (or Thunderbolt 4) cable, and gives both ends a 169.254.x.x address. WireView runs over that link.

- **No compression.** Pixels go over the wire as raw 32-bit BGRA, exactly as Windows captured them.
- **Only what changed.** Windows' desktop capture (DXGI Desktop Duplication) reports which rectangles changed and which were moved (scrolling). WireView sends just those rectangles, and moves as 24-byte copy instructions. A still screen costs nothing.
- **No CPU copies on the sending side.** Changed rectangles are copied on the GPU into a readable texture, and the socket reads them straight from that memory.
- **Upload only changed rectangles on the receiving side,** draw with Direct3D 11, never queue frames.
- **Control** is a small side channel: 12-byte mouse and keyboard events, replayed with `SendInput` on the shared PC.

Bandwidth for full-screen motion is roughly width x height x 4 x fps: a 1920x1080 screen at 60 fps is about 4 Gbit/s. A typical desktop is far less because only changed areas are sent. USB4 links are 20 or 40 Gbit/s; that is the link rate, and what TCP actually gets over Windows' USB4 network adapter has not been measured yet. The status bar shows the live frame rate and Gbit/s on both sides.

Any IPv4 network works too (Ethernet, Wi-Fi), but at Wi-Fi speeds raw frames will be slow when a lot of the screen changes.

## Requirements

- Windows 10 or 11, 64-bit.
- For the cable: USB4 or Thunderbolt 4 ports on **both** PCs, Windows 11, and a USB4 / Thunderbolt cable. A plain USB-C cable between two normal USB-C ports does not create a link.

## Use

On the PC to be shared:

1. **Share > Share this screen.**
2. To let the other PC control it, also tick **Share > Allow remote control**.
3. The first time, Windows Firewall asks about network access. The USB4 link counts as an "unidentified network", which Windows treats as **Public**, so allow Public networks. Or use **Share > Allow through firewall...** (needs admin), which allows WireView on all network types.

On the other PC:

1. **File > Connect...** It lists PCs that are sharing, USB4 links first. Double-click one, or type an address.
2. Keep **Ask to control the other PC** ticked if you want control.
3. The shared PC asks whether to let you in.
4. **Control > Take control** (Ctrl+Alt+F12) sends your mouse and keyboard to the other PC, including Alt+Tab and the Windows key. Press **Ctrl+Alt+F12** again to get them back.

Other options: **View > Full screen** (F11), **View > Refresh** (F5) to resend the whole screen, **Share > Monitor** to pick which screen to share. Settings are kept in `HKEY_CURRENT_USER\Software\WireView`.

Command line: `WireView.exe --share` starts sharing, `WireView.exe --connect 169.254.10.20` connects.

## Limits

- Not tested on USB4 hardware yet. It was developed and tested on one PC over loopback. Two-PC control is untested.
- Not encrypted. It is meant for a cable between two PCs you own. Without "Ask before accepting", anyone who can reach the port can see the screen.
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

`wireview_tests.exe` checks the protocol parser, input mapping, and GPU frame updates against a CPU reference. It also runs a real capture session over loopback; input there goes to a recorder, never to the real desktop. `wireview_tests.exe --bench` measures loopback throughput and capture speed.

There are no dependencies beyond the Windows SDK. The C runtime is linked statically, so `WireView.exe` is a single file.

## Layout

```
src/protocol.*   wire format and strict parsing
src/net.*        sockets, adapters
src/discovery.*  finding sharing PCs (UDP broadcast)
src/capture.*    DXGI Desktop Duplication
src/host.*       the sharing side: capture, send, receive input
src/inject.*     input events -> SendInput
src/viewer.*     the viewing side: receive, send input
src/render.*     Direct3D 11 drawing
src/cursor.*     remote pointer shapes
src/main.cpp     the window, menus and dialogs
docs/protocol.md the protocol
```

## License

GPL-3.0. See [LICENSE](LICENSE).
