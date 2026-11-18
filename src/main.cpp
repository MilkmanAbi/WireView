// WireView: see and control another Windows PC over a USB4 cable (or any network).
// Plain Win32: a menu, a status bar, and the other PC's screen in the window.
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>

#include "cursor.h"
#include "discovery.h"
#include "host.h"
#include "net.h"
#include "render.h"
#include "resource.h"
#include "util.h"
#include "viewer.h"

using namespace wv;

namespace {

constexpr UINT WM_APP_VIEWER = WM_APP + 1;  // wParam = ViewerConn::Event
constexpr UINT WM_APP_HOST = WM_APP + 2;    // wParam = Host::Event, lParam = std::string*
constexpr UINT WM_APP_ASK = WM_APP + 3;     // lParam = AskRequest*, returns 1 to accept
constexpr UINT_PTR kStatsTimer = 1;
const wchar_t kMainClass[] = L"WireViewMain";
const wchar_t kViewClass[] = L"WireViewView";
const wchar_t kRegKey[] = L"Software\\WireView";
const char kVersion[] = "0.2.0";

// How the picture is sent. maxHeight 0 = native size, quality 0 = raw pixels.
struct Preset {
    const wchar_t* name;
    uint32_t maxHeight, quality;
};
const Preset kPresets[] = {
    {L"Automatic (raw on a USB4 link, else compressed 720p)", 0, 0},
    {L"Raw, full resolution (USB4)", 0, 0},
    {L"Compressed, full resolution", 0, 85},
    {L"Compressed, 1080p", 1080, 80},
    {L"Compressed, 720p (Wi-Fi, hotspot, slow links)", 720, 75},
};
constexpr DWORD kPresetCount = DWORD(sizeof kPresets / sizeof kPresets[0]);

struct AskRequest {
    std::string address, name;
    bool wantsControl;
};

struct Settings {
    std::wstring address;
    DWORD port = kTcpPort;
    bool allowControl = false;
    bool askAccept = true;
    bool shareOnStart = false;
    bool wantControl = true;
    bool showPointer = true;
    DWORD monitor = 0;
    DWORD picture = 0;  // index into kPresets
};

struct App {
    HINSTANCE inst = nullptr;
    HWND main = nullptr, view = nullptr, status = nullptr;
    HACCEL accel = nullptr;
    Settings cfg;

    SendInputSink inputSink;
    std::unique_ptr<Host> host;
    DiscoveryResponder responder;
    ViewerConn viewer;
    Renderer renderer;

    bool viewing = false;   // connected and showing the remote screen
    bool granted = false;   // the host lets us control
    bool controlling = false;
    bool shuttingDown = false;
    bool stoppingHost = false;
    std::string viewText;
    uint32_t nativeW = 0, nativeH = 0, streamFormat = 0;
    bool directLink = false;  // the current connection runs over a USB4 / Thunderbolt adapter
    std::string shareError;

    HCURSOR remoteCursor = nullptr;
    bool remoteCursorVisible = true;
    HHOOK kbHook = nullptr;
    unsigned buttonsDown = 0;

    bool fullscreen = false;
    WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR savedStyle = 0;
    HMENU savedMenu = nullptr;

    uint64_t framesShown = 0, lastFrames = 0, lastViewBytes = 0, lastHostFrames = 0, lastHostBytes = 0;
    ULONGLONG lastTick = 0;
    double viewFps = 0, viewBps = 0, hostFps = 0, hostBps = 0;
};
App g;

// ------------------------------------------------------------------ settings

DWORD reg_dword(const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}
std::wstring reg_string(const wchar_t* name) {
    wchar_t buf[512];
    DWORD size = sizeof buf;
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
}
void reg_set(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &v, sizeof v);
}
void reg_set(const wchar_t* name, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

void load_settings() {
    Settings& c = g.cfg;
    c.address = reg_string(L"Address");
    c.port = reg_dword(L"Port", kTcpPort);
    if (c.port == 0 || c.port > 65535) c.port = kTcpPort;
    c.allowControl = reg_dword(L"AllowControl", 0) != 0;
    c.askAccept = reg_dword(L"AskAccept", 1) != 0;
    c.shareOnStart = reg_dword(L"ShareOnStart", 0) != 0;
    c.wantControl = reg_dword(L"WantControl", 1) != 0;
    c.showPointer = reg_dword(L"ShowPointer", 1) != 0;
    c.monitor = reg_dword(L"Monitor", 0);
    c.picture = reg_dword(L"Picture", 0);
    if (c.picture >= kPresetCount) c.picture = 0;
}

void save_settings() {
    const Settings& c = g.cfg;
    reg_set(L"Address", c.address);
    reg_set(L"Port", c.port);
    reg_set(L"AllowControl", c.allowControl);
    reg_set(L"AskAccept", c.askAccept);
    reg_set(L"ShareOnStart", c.shareOnStart);
    reg_set(L"WantControl", c.wantControl);
    reg_set(L"ShowPointer", c.showPointer);
    reg_set(L"Monitor", c.monitor);
    reg_set(L"Picture", c.picture);
}

// ------------------------------------------------------------------ helpers

// Waits for a worker thread while still answering window messages, so a
// worker blocked in SendMessage to us can finish.
void join_pumping(std::thread& t) {
    if (!t.joinable()) return;
    // Own handle: a message handled below may join this same thread (re-entrantly)
    // and close std::thread's handle while we still wait on it.
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), t.native_handle(), GetCurrentProcess(), &h, SYNCHRONIZE, FALSE, 0)) {
        t.join();
        return;
    }
    bool quit = false;
    int quitCode = 0;
    for (;;) {
        const DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1) break;  // thread ended (or the wait failed)
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                quitCode = int(msg.wParam);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CloseHandle(h);
    if (t.joinable()) t.join();
    if (quit) PostQuitMessage(quitCode);
}

void set_status(int part, const std::string& text) {
    SendMessageW(g.status, SB_SETTEXTW, WPARAM(part), LPARAM(to_wide(text).c_str()));
}

void error_box(const std::string& text) {
    MessageBoxW(g.main, to_wide(text).c_str(), L"WireView", MB_OK | MB_ICONWARNING);
}

void layout() {
    RECT rc;
    GetClientRect(g.main, &rc);
    int statusH = 0;
    if (!g.fullscreen) {
        SendMessageW(g.status, WM_SIZE, 0, 0);
        RECT sr;
        GetWindowRect(g.status, &sr);
        statusH = sr.bottom - sr.top;
        const int w = rc.right;
        int parts[2] = {w * 60 / 100, -1};
        SendMessageW(g.status, SB_SETPARTS, 2, LPARAM(parts));
    }
    MoveWindow(g.view, 0, 0, rc.right, std::max<LONG>(rc.bottom - statusH, 1), TRUE);
}

void update_title() {
    std::wstring t = L"WireView";
    if (g.viewing) t += L" - " + to_wide(g.viewer.remote_name());
    if (g.controlling) t += L" (controlling, Ctrl+Alt+F12 to release)";
    SetWindowTextW(g.main, t.c_str());
}

// True if address is an IPv4 literal reached through a USB4 / Thunderbolt adapter.
bool is_direct_link(const std::string& address) {
    in_addr a{};
    if (inet_pton(AF_INET, address.c_str(), &a) != 1) return false;
    const auto adapters = list_adapters();
    const Adapter* ad = adapter_for(adapters, a.s_addr);
    return ad && ad->directLink;
}

StreamSettingsMsg picture_settings(DWORD preset, bool directLink) {
    if (preset >= kPresetCount) preset = 0;
    if (preset == 0) return directLink ? StreamSettingsMsg{0, 0} : StreamSettingsMsg{kPresets[4].maxHeight, kPresets[4].quality};
    return {kPresets[preset].maxHeight, kPresets[preset].quality};
}

// ------------------------------------------------------------------ control

LRESULT CALLBACK keyboard_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g.controlling && GetForegroundWindow() == g.main) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        // Injected keys are not the user's (and on a same-PC test they'd loop back).
        if (!(k->flags & LLKHF_INJECTED)) {
            const bool up = (k->flags & LLKHF_UP) != 0;
            if (k->vkCode == VK_F12 && !up && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
                PostMessageW(g.main, WM_COMMAND, IDM_TAKE_CONTROL, 0);
                return 1;
            }
            if (k->scanCode != 0 && k->scanCode < 0x100) {
                InputEvent e{kInKey, uint8_t((up ? kKeyUp : 0) | ((k->flags & LLKHF_EXTENDED) ? kKeyExtended : 0)),
                             uint16_t(k->scanCode), 0, 0};
                g.viewer.send_input(&e, 1);
                return 1;  // swallow: Alt+Tab, Win key etc. go to the other PC
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void release_remote_input() {
    if (!g.viewing) return;
    InputEvent e{kInReleaseAll, 0, 0, 0, 0};
    g.viewer.send_input(&e, 1);
}

void set_controlling(bool on) {
    if (on && !(g.viewing && g.granted)) on = false;
    if (on == g.controlling) return;
    g.controlling = on;
    if (on) {
        g.kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, g.inst, 0);
        SetFocus(g.view);
    } else {
        if (g.kbHook) UnhookWindowsHookEx(g.kbHook);
        g.kbHook = nullptr;
        if (GetCapture() == g.view) ReleaseCapture();
        g.buttonsDown = 0;
        release_remote_input();
    }
    g.renderer.set_draw_pointer(!on && g.cfg.showPointer);
    update_title();
    InvalidateRect(g.view, nullptr, FALSE);
}

bool to_remote(int cx, int cy, int32_t& rx, int32_t& ry) {
    const RECT r = g.renderer.image_rect();
    // Input is always in the other PC's native pixels, whatever size the picture is streamed at.
    const uint32_t w = g.nativeW, h = g.nativeH;
    if (r.right <= r.left || r.bottom <= r.top || !w || !h) return false;
    rx = int32_t((int64_t(cx - r.left) * w) / (r.right - r.left));
    ry = int32_t((int64_t(cy - r.top) * h) / (r.bottom - r.top));
    rx = std::clamp<int32_t>(rx, 0, int32_t(w) - 1);
    ry = std::clamp<int32_t>(ry, 0, int32_t(h) - 1);
    return true;
}

void send_mouse(uint8_t kind, uint8_t a, uint16_t b, LPARAM lp) {
    int32_t x, y;
    if (!to_remote(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, y)) return;
    InputEvent e{kind, a, b, x, y};
    g.viewer.send_input(&e, 1);
}

// ------------------------------------------------------------------ viewing

void apply_packets() {
    std::deque<ViewerConn::Packet> q;
    g.viewer.take(q);
    for (auto& p : q) {
        switch (p.type) {
            case kMsgMode: {
                ModeMsg m;
                memcpy(&m, p.data.data(), sizeof m);
                std::string err;
                if (!g.renderer.image().set_mode(m.width, m.height, err)) g.viewText = err;
                g.nativeW = m.nativeWidth;
                g.nativeH = m.nativeHeight;
                g.streamFormat = m.format;
                g.renderer.set_pointer_scale(m.nativeWidth ? float(m.width) / float(m.nativeWidth) : 1.f);
                break;
            }
            case kMsgFrame: {
                FrameView f;
                if (parse_frame(p.data.data(), p.data.size(), g.renderer.image().width(), g.renderer.image().height(), f)) {
                    g.renderer.image().apply(f);
                    ++g.framesShown;
                }
                break;
            }
            case kMsgPointerPos: {
                PointerPosMsg m;
                memcpy(&m, p.data.data(), sizeof m);
                g.renderer.set_pointer_pos(m.x, m.y, m.visible != 0);
                g.remoteCursorVisible = m.visible != 0;
                break;
            }
            case kMsgPointerShape: {
                PointerShapeMsg h;
                const uint8_t* d = nullptr;
                if (parse_pointer_shape(p.data.data(), p.data.size(), h, d)) {
                    g.renderer.set_pointer_shape(h, d);
                    if (g.remoteCursor) DestroyCursor(g.remoteCursor);
                    g.remoteCursor = pointer_to_hcursor(h, d);
                    if (g.controlling) {
                        POINT pt;
                        GetCursorPos(&pt);
                        if (WindowFromPoint(pt) == g.view) SetCursor(g.remoteCursor);
                    }
                }
                break;
            }
            case kMsgControl: {
                ControlMsg m;
                memcpy(&m, p.data.data(), sizeof m);
                g.granted = m.granted != 0;
                if (!g.granted) set_controlling(false);
                break;
            }
        }
        g.viewer.recycle(std::move(p.data));
    }
    if (!g.renderer.draw()) {
        // GPU device lost (driver update, TDR): rebuild and ask for a full frame.
        const uint32_t w = g.renderer.image().width(), h = g.renderer.image().height();
        std::string err;
        if (g.renderer.init(g.view, err) && w && h) g.renderer.image().set_mode(w, h, err);
        g.viewer.request_full();
    }
}

void disconnect() {
    set_controlling(false);
    g.viewer.stop(join_pumping);
    g.viewing = false;
    g.granted = false;
    g.nativeW = g.nativeH = g.streamFormat = 0;
    g.renderer.image().reset();
    ShowWindow(g.view, SW_HIDE);
    InvalidateRect(g.main, nullptr, TRUE);
    update_title();
}

void connect_to(const std::wstring& address, uint16_t port, bool wantControl) {
    disconnect();
    g.viewText = "Connecting to " + to_utf8(address) + "...";
    set_status(0, g.viewText);
    HWND hwnd = g.main;
    g.directLink = is_direct_link(to_utf8(address));
    const StreamSettingsMsg st = picture_settings(g.cfg.picture, g.directLink);
    g.viewer.start(to_utf8(address), port, wantControl, st.maxHeight, st.quality,
                   [hwnd](ViewerConn::Event e) { PostMessageW(hwnd, WM_APP_VIEWER, WPARAM(e), 0); });
}

void on_viewer_event(ViewerConn::Event e) {
    if (g.shuttingDown) return;
    switch (e) {
        case ViewerConn::Event::Connected: {
            if (!g.renderer.ready()) {
                std::string err;
                if (!g.renderer.init(g.view, err)) {
                    error_box("Cannot start Direct3D: " + err);
                    disconnect();
                    return;
                }
            }
            g.renderer.set_background(GetSysColor(COLOR_APPWORKSPACE));
            g.renderer.set_draw_pointer(g.cfg.showPointer);
            g.viewing = true;
            g.granted = g.viewer.control_granted_at_start();
            g.framesShown = g.lastFrames = 0;
            g.lastViewBytes = 0;
            std::string via;
            in_addr a{};
            if (inet_pton(AF_INET, g.viewer.remote_address().c_str(), &a) == 1) {
                const auto adapters = list_adapters();
                if (const Adapter* ad = adapter_for(adapters, a.s_addr))
                    via = std::string(" via ") + (ad->directLink ? "USB4 link" : ad->name);
            }
            g.viewText = "Viewing " + g.viewer.remote_name() + " (" + g.viewer.remote_address() + ")" + via;
            ShowWindow(g.view, SW_SHOW);
            layout();
            g.renderer.resize();
            update_title();
            break;
        }
        case ViewerConn::Event::Failed:
        case ViewerConn::Event::Closed: {
            const std::string err = g.viewer.error();
            const bool wasViewing = g.viewing;
            disconnect();
            g.viewText = err.empty() ? "Disconnected" : err;
            set_status(0, g.viewText);
            if (!wasViewing && !err.empty() && err != "Cancelled") error_box(err);
            break;
        }
        case ViewerConn::Event::Packets:
            if (g.viewing) apply_packets();
            break;
    }
}

// ------------------------------------------------------------------ sharing

DiscoveryReply discovery_info() {
    DiscoveryReply r{};
    r.tcpPort = uint16_t(g.cfg.port);
    r.flags = g.cfg.allowControl ? kDiscoveryControlAllowed : 0;
    fill_name(r.name, computer_name_utf8().c_str());
    return r;
}

void stop_sharing() {
    g.responder.stop();
    g.stoppingHost = true;  // a viewer waiting at the accept prompt is turned away
    if (g.host) g.host->stop(join_pumping);
    g.stoppingHost = false;
}

void start_sharing() {
    if (!g.host) {
        g.host = std::make_unique<Host>(g.inputSink);
        HWND hwnd = g.main;
        g.host->set_events([hwnd](Host::Event e, const std::string& text) {
            PostMessageW(hwnd, WM_APP_HOST, WPARAM(e), LPARAM(new std::string(text)));
        });
        g.host->set_accept([hwnd](const std::string& address, const std::string& name, bool wantsControl) {
            AskRequest req{address, name, wantsControl};
            return SendMessageW(hwnd, WM_APP_ASK, 0, LPARAM(&req)) == 1;
        });
    }
    g.host->set_allow_control(g.cfg.allowControl);
    g.host->set_monitor(int(g.cfg.monitor));
    std::string err;
    if (!g.host->start(uint16_t(g.cfg.port), err)) {
        g.shareError = err;
        error_box(err);
        return;
    }
    g.shareError.clear();
    g.responder.start(discovery_info);  // not fatal if UDP is unavailable: the address can be typed
}

bool sharing() { return g.host && g.host->running(); }

void allow_through_firewall() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // A USB4 link shows up as an "unidentified network", which Windows treats as Public.
    std::wstring args = L"/c netsh advfirewall firewall delete rule name=\"WireView\" >nul & "
                        L"netsh advfirewall firewall add rule name=\"WireView\" dir=in action=allow program=\"";
    args += exe;
    args += L"\" enable=yes profile=any";
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g.main;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return;  // cancelled at the UAC prompt
    WaitForSingleObject(sei.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    MessageBoxW(g.main,
                code == 0 ? L"WireView is allowed through Windows Firewall on all network types."
                          : L"Adding the firewall rule failed.",
                L"WireView", MB_OK | (code == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
}

// ------------------------------------------------------------------ connect dialog

struct ConnectState {
    std::vector<Peer> peers;
};

void fill_peers(HWND dlg, ConnectState& st) {
    SetDlgItemTextW(dlg, IDC_SEARCH_STATUS, L"Searching...");
    SendDlgItemMessageW(dlg, IDC_PEERS, LB_RESETCONTENT, 0, 0);
    UpdateWindow(dlg);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    st.peers = discover_peers(700);
    SetCursor(old);
    for (const Peer& p : st.peers) {
        std::string line = p.name + "   " + p.address;
        if (p.port != kTcpPort) line += ":" + std::to_string(p.port);
        if (p.directLink)
            line += "   [USB4" + (p.linkBps ? ", " + format_bps(double(p.linkBps)) : std::string()) + "]";
        else if (!p.adapter.empty())
            line += "   [" + p.adapter + "]";
        if (!p.controlAllowed) line += "   (view only)";
        SendDlgItemMessageW(dlg, IDC_PEERS, LB_ADDSTRING, 0, LPARAM(to_wide(line).c_str()));
    }
    SetDlgItemTextW(dlg, IDC_SEARCH_STATUS,
                    st.peers.empty() ? L"None found. Type the address instead." : L"Double-click one to connect.");
}

INT_PTR CALLBACK connect_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ConnectState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG:
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            st = reinterpret_cast<ConnectState*>(lp);
            SetDlgItemTextW(dlg, IDC_ADDRESS, g.cfg.address.c_str());
            SetDlgItemInt(dlg, IDC_PORT, g.cfg.port, FALSE);
            CheckDlgButton(dlg, IDC_WANT_CONTROL, g.cfg.wantControl ? BST_CHECKED : BST_UNCHECKED);
            for (const Preset& pr : kPresets) SendDlgItemMessageW(dlg, IDC_PICTURE, CB_ADDSTRING, 0, LPARAM(pr.name));
            SendDlgItemMessageW(dlg, IDC_PICTURE, CB_SETCURSEL, g.cfg.picture, 0);
            PostMessageW(dlg, WM_COMMAND, IDC_SEARCH, 0);
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_SEARCH:
                    fill_peers(dlg, *st);
                    return TRUE;
                case IDC_PEERS: {
                    const int sel = int(SendDlgItemMessageW(dlg, IDC_PEERS, LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < int(st->peers.size())) {
                        SetDlgItemTextW(dlg, IDC_ADDRESS, to_wide(st->peers[size_t(sel)].address).c_str());
                        SetDlgItemInt(dlg, IDC_PORT, st->peers[size_t(sel)].port, FALSE);
                        if (HIWORD(wp) == LBN_DBLCLK) PostMessageW(dlg, WM_COMMAND, IDOK, 0);
                    }
                    return TRUE;
                }
                case IDOK: {
                    wchar_t addr[256] = {};
                    GetDlgItemTextW(dlg, IDC_ADDRESS, addr, 256);
                    BOOL ok = FALSE;
                    const UINT port = GetDlgItemInt(dlg, IDC_PORT, &ok, FALSE);
                    std::wstring a = addr;
                    while (!a.empty() && iswspace(a.back())) a.pop_back();
                    while (!a.empty() && iswspace(a.front())) a.erase(a.begin());
                    if (a.empty() || !ok || port == 0 || port > 65535) {
                        MessageBoxW(dlg, L"Enter the other PC's address and a port between 1 and 65535.", L"WireView",
                                    MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                    g.cfg.address = a;
                    g.cfg.port = port;
                    g.cfg.wantControl = IsDlgButtonChecked(dlg, IDC_WANT_CONTROL) == BST_CHECKED;
                    const LRESULT pic = SendDlgItemMessageW(dlg, IDC_PICTURE, CB_GETCURSEL, 0, 0);
                    if (pic >= 0 && pic < LRESULT(kPresetCount)) g.cfg.picture = DWORD(pic);
                    save_settings();
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// ------------------------------------------------------------------ menus

void fill_monitor_menu(HMENU menu) {
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    const auto monitors = list_monitors();
    if (monitors.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_MONITOR_BASE, L"(none)");
    for (size_t i = 0; i < monitors.size() && i < 32; ++i) {
        const std::wstring text = L"&" + std::to_wstring(i + 1) + L"  " + to_wide(monitors[i].name);
        AppendMenuW(menu, MF_STRING | (i == g.cfg.monitor ? MF_CHECKED : 0), UINT(IDM_MONITOR_BASE + i), text.c_str());
    }
}

void fill_picture_menu(HMENU menu) {
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    for (DWORD i = 0; i < kPresetCount; ++i) {
        AppendMenuW(menu, MF_STRING, IDM_PICTURE_BASE + i, kPresets[i].name);
        if (i == 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    CheckMenuRadioItem(menu, IDM_PICTURE_BASE, IDM_PICTURE_BASE + kPresetCount - 1, IDM_PICTURE_BASE + g.cfg.picture,
                       MF_BYCOMMAND);
}

void update_menu(HMENU m) {
    auto check = [m](UINT id, bool on) { CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    auto enable = [m](UINT id, bool on) { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    enable(IDM_DISCONNECT, g.viewer.active());
    check(IDM_SHARE, sharing());
    check(IDM_ALLOW_CONTROL, g.cfg.allowControl);
    check(IDM_ASK, g.cfg.askAccept);
    check(IDM_SHARE_STARTUP, g.cfg.shareOnStart);
    enable(IDM_KICK, sharing() && g.host->has_viewer());
    check(IDM_FULLSCREEN, g.fullscreen);
    check(IDM_SHOW_POINTER, g.cfg.showPointer);
    enable(IDM_REFRESH, g.viewing);
    check(IDM_TAKE_CONTROL, g.controlling);
    enable(IDM_TAKE_CONTROL, g.viewing && g.granted);
}

void set_fullscreen(bool on) {
    if (on == g.fullscreen) return;
    g.fullscreen = on;
    if (on) {
        GetWindowPlacement(g.main, &g.savedPlacement);
        g.savedStyle = GetWindowLongPtrW(g.main, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(g.main, MONITOR_DEFAULTTONEAREST), &mi);
        g.savedMenu = GetMenu(g.main);
        SetMenu(g.main, nullptr);
        ShowWindow(g.status, SW_HIDE);
        SetWindowLongPtrW(g.main, GWL_STYLE, (g.savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g.main, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(g.main, GWL_STYLE, g.savedStyle);
        SetMenu(g.main, g.savedMenu);
        ShowWindow(g.status, SW_SHOW);
        SetWindowPlacement(g.main, &g.savedPlacement);
        SetWindowPos(g.main, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    layout();
}

void on_command(UINT id) {
    switch (id) {
        case IDM_CONNECT: {
            ConnectState st;
            if (DialogBoxParamW(g.inst, MAKEINTRESOURCEW(IDD_CONNECT), g.main, connect_proc, LPARAM(&st)) == IDOK)
                connect_to(g.cfg.address, uint16_t(g.cfg.port), g.cfg.wantControl);
            break;
        }
        case IDM_DISCONNECT:
            disconnect();
            g.viewText = "Disconnected";
            break;
        case IDM_EXIT:
            DestroyWindow(g.main);
            break;
        case IDM_SHARE:
            if (sharing())
                stop_sharing();
            else
                start_sharing();
            InvalidateRect(g.main, nullptr, TRUE);
            break;
        case IDM_ALLOW_CONTROL:
            g.cfg.allowControl = !g.cfg.allowControl;
            if (g.host) g.host->set_allow_control(g.cfg.allowControl);
            save_settings();
            break;
        case IDM_ASK:
            if (g.cfg.askAccept &&
                MessageBoxW(g.main,
                            L"Without asking, anyone who can reach this PC on the sharing port can see your screen"
                            L" (and control it, if remote control is allowed).\n\nOnly do this on a direct cable"
                            L" or a network you trust. Continue?",
                            L"WireView", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                break;
            g.cfg.askAccept = !g.cfg.askAccept;
            save_settings();
            break;
        case IDM_SHARE_STARTUP:
            g.cfg.shareOnStart = !g.cfg.shareOnStart;
            save_settings();
            break;
        case IDM_KICK:
            if (g.host) g.host->disconnect_viewer();
            break;
        case IDM_FIREWALL:
            allow_through_firewall();
            break;
        case IDM_FULLSCREEN:
            set_fullscreen(!g.fullscreen);
            break;
        case IDM_SHOW_POINTER:
            g.cfg.showPointer = !g.cfg.showPointer;
            g.renderer.set_draw_pointer(!g.controlling && g.cfg.showPointer);
            InvalidateRect(g.view, nullptr, FALSE);
            save_settings();
            break;
        case IDM_REFRESH:
            if (g.viewing) g.viewer.request_full();
            break;
        case IDM_TAKE_CONTROL:
            if (!g.controlling && g.viewing && !g.granted) {
                MessageBoxW(g.main,
                            L"The other PC does not allow control.\n\nOn that PC tick Share > Allow remote control,"
                            L" and here connect with \"Ask to control the other PC\" ticked.",
                            L"WireView", MB_OK | MB_ICONINFORMATION);
                break;
            }
            set_controlling(!g.controlling);
            break;
        case IDM_ABOUT: {
            std::wstring text = L"WireView " + to_wide(kVersion) +
                                L"\n\nScreen view and control between two Windows PCs: raw pixels over a USB4"
                                L" cable, or compressed over slower links.\n\nFree software under the GNU GPL v3.\n"
                                L"https://github.com/MilkmanAbi/WireView";
            MessageBoxW(g.main, text.c_str(), L"About WireView", MB_OK | MB_ICONINFORMATION);
            break;
        }
        default:
            if (id >= IDM_PICTURE_BASE && id < IDM_PICTURE_BASE + kPresetCount) {
                g.cfg.picture = id - IDM_PICTURE_BASE;
                save_settings();
                if (g.viewing) {
                    const StreamSettingsMsg st = picture_settings(g.cfg.picture, g.directLink);
                    g.viewer.set_stream(st.maxHeight, st.quality);
                }
                break;
            }
            if (id >= IDM_MONITOR_BASE && id < IDM_MONITOR_BASE + 32) {
                g.cfg.monitor = id - IDM_MONITOR_BASE;
                if (g.host) g.host->set_monitor(int(g.cfg.monitor));
                save_settings();
            }
    }
}

void update_stats() {
    const ULONGLONG now = GetTickCount64();
    const double dt = g.lastTick ? (now - g.lastTick) / 1000.0 : 0;
    g.lastTick = now;
    if (dt > 0) {
        const uint64_t vb = g.viewer.bytes_received();
        g.viewFps = (g.framesShown - g.lastFrames) / dt;
        g.viewBps = (vb - g.lastViewBytes) * 8 / dt;
        g.lastFrames = g.framesShown;
        g.lastViewBytes = vb;
        if (g.host) {
            const HostStats s = g.host->stats();
            g.hostFps = (s.frames - g.lastHostFrames) / dt;
            g.hostBps = (s.bytes - g.lastHostBytes) * 8 / dt;
            g.lastHostFrames = s.frames;
            g.lastHostBytes = s.bytes;
        }
    }
    char buf[96];
    std::string left = g.viewText.empty() ? "Not connected" : g.viewText;
    if (g.viewing) {
        std::snprintf(buf, sizeof buf, "  |  %ux%u %s  %.0f fps  ", g.renderer.image().width(),
                      g.renderer.image().height(), g.streamFormat == kFormatEncoded ? "compressed" : "raw", g.viewFps);
        left += buf + format_bps(g.viewBps);
        left += g.controlling ? "  |  Controlling" : g.granted ? "  |  Control available" : "  |  View only";
    }
    set_status(0, left);

    std::string right;
    if (!sharing()) {
        right = g.shareError.empty() ? "Not sharing" : "Sharing failed: " + g.shareError;
    } else if (g.host->has_viewer()) {
        std::snprintf(buf, sizeof buf, "  %.0f fps  ", g.hostFps);
        right = "Shared with " + g.host->viewer_text() + buf + format_bps(g.hostBps);
    } else {
        right = "Sharing on port " + std::to_string(g.cfg.port) + ", waiting";
    }
    if (!g.shareError.empty() && sharing()) right += "  (" + g.shareError + ")";
    set_status(1, right);
}

void paint_idle(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_APPWORKSPACE));
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, GetDpiForWindow(hwnd));
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOW));
    std::wstring text =
        L"Not connected.\n\nFile > Connect... to view another PC.\nShare > Share this screen to let another PC view this one.";
    if (sharing()) text += L"\n\nThis screen is being shared on port " + std::to_wstring(g.cfg.port) + L".";
    RECT tr = rc;
    DrawTextW(dc, text.c_str(), -1, &tr, DT_CENTER | DT_CALCRECT | DT_WORDBREAK);
    const int h = tr.bottom - tr.top;
    tr = rc;
    tr.top = (rc.bottom - h) / 2;
    DrawTextW(dc, text.c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK);
    SelectObject(dc, old);
    DeleteObject(font);
    EndPaint(hwnd, &ps);
}

// ------------------------------------------------------------------ window procs

LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            if (g.viewing) g.renderer.draw();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (g.renderer.ready()) {
                g.renderer.resize();
                g.renderer.draw();
            }
            return 0;
        case WM_SETCURSOR:
            if (g.controlling && LOWORD(lp) == HTCLIENT) {
                SetCursor(g.remoteCursorVisible ? (g.remoteCursor ? g.remoteCursor : LoadCursorW(nullptr, IDC_ARROW))
                                                : nullptr);
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            if (g.controlling) send_mouse(kInMove, 0, 0, lp);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            if (!g.controlling) return 0;
            uint8_t button = 0;
            bool down = false;
            switch (msg) {
                case WM_LBUTTONDOWN: down = true; [[fallthrough]];
                case WM_LBUTTONUP: button = 0; break;
                case WM_RBUTTONDOWN: down = true; [[fallthrough]];
                case WM_RBUTTONUP: button = 1; break;
                case WM_MBUTTONDOWN: down = true; [[fallthrough]];
                case WM_MBUTTONUP: button = 2; break;
                case WM_XBUTTONDOWN: down = true; [[fallthrough]];
                default: button = GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? 4 : 3; break;
            }
            if (down) {
                SetFocus(hwnd);
                if (!g.buttonsDown) SetCapture(hwnd);
                g.buttonsDown |= 1u << button;
            } else {
                g.buttonsDown &= ~(1u << button);
                if (!g.buttonsDown && GetCapture() == hwnd) ReleaseCapture();
            }
            send_mouse(kInButton, button, down ? 1 : 0, lp);
            return (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP) ? TRUE : 0;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (g.controlling) {
                InputEvent e{kInWheel, uint8_t(msg == WM_MOUSEHWHEEL ? 1 : 0), 0, GET_WHEEL_DELTA_WPARAM(wp), 0};
                g.viewer.send_input(&e, 1);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (g.buttonsDown && g.controlling) {
                g.buttonsDown = 0;
                release_remote_input();
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.main = hwnd;
            g.status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                       hwnd, nullptr, g.inst, nullptr);
            g.view = CreateWindowExW(0, kViewClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, hwnd, nullptr,
                                     g.inst, nullptr);
            SetTimer(hwnd, kStatsTimer, 1000, nullptr);
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_PAINT:
            paint_idle(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_INITMENUPOPUP: {
            HMENU menu = HMENU(wp);
            const UINT first = GetMenuItemID(menu, 0);
            if (first >= IDM_MONITOR_BASE && first < IDM_MONITOR_BASE + 32) fill_monitor_menu(menu);
            if (first >= IDM_PICTURE_BASE && first < IDM_PICTURE_BASE + kPresetCount) fill_picture_menu(menu);
            update_menu(GetMenu(hwnd));
            return 0;
        }
        case WM_COMMAND:
            on_command(LOWORD(wp));
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (g.controlling) return view_proc(g.view, msg, wp, lp);
            break;
        case WM_ACTIVATE:
            // Leaving the window: let go of anything held on the other PC so no key sticks.
            if (LOWORD(wp) == WA_INACTIVE && g.controlling) release_remote_input();
            if (LOWORD(wp) != WA_INACTIVE && g.viewing) SetFocus(g.view);
            return 0;
        case WM_TIMER:
            if (wp == kStatsTimer) update_stats();
            return 0;
        case WM_APP_VIEWER:
            on_viewer_event(ViewerConn::Event(wp));
            return 0;
        case WM_APP_HOST: {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
            const auto e = Host::Event(wp);
            if (e == Host::Event::Error)
                g.shareError = *text;
            else
                g.shareError.clear();
            if (e == Host::Event::ViewerConnected) FlashWindow(hwnd, TRUE);
            update_stats();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_APP_ASK: {
            const auto* req = reinterpret_cast<const AskRequest*>(lp);
            if (g.shuttingDown || g.stoppingHost) return 0;
            if (!g.cfg.askAccept) return 1;
            std::wstring text = to_wide(req->name.empty() ? std::string("A PC") : req->name) + L" (" +
                                to_wide(req->address) + L") wants to see this screen";
            text += req->wantsControl && g.cfg.allowControl ? L" and control it." : L".";
            text += L"\n\nAllow?";
            FlashWindow(hwnd, TRUE);
            return MessageBoxW(hwnd, text.c_str(), L"WireView", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 |
                                                                   MB_SETFOREGROUND) == IDYES;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g.shuttingDown = true;
            KillTimer(hwnd, kStatsTimer);
            set_controlling(false);
            g.viewer.stop(join_pumping);
            stop_sharing();
            g.renderer.shutdown();
            if (g.remoteCursor) DestroyCursor(g.remoteCursor);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g.inst = inst;
    WinsockInit winsock;
    if (!winsock.ok) {
        MessageBoxW(nullptr, L"Winsock failed to start.", L"WireView", MB_OK | MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    load_settings();

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = view_proc;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = kViewClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kMainClass, L"WireView", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 800, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    g.accel = LoadAcceleratorsW(inst, MAKEINTRESOURCEW(IDR_ACCEL));
    ShowWindow(hwnd, show);
    update_stats();

    // Command line: --share, --connect <address>
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool share = g.cfg.shareOnStart;
    std::wstring connectAddr;
    for (int i = 1; argv && i < argc; ++i) {
        if (!lstrcmpiW(argv[i], L"--share")) share = true;
        if (!lstrcmpiW(argv[i], L"--connect") && i + 1 < argc) connectAddr = argv[++i];
    }
    if (argv) LocalFree(argv);
    if (share) start_sharing();
    if (!connectAddr.empty()) connect_to(connectAddr, uint16_t(g.cfg.port), g.cfg.wantControl);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, g.accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return int(msg.wParam);
}
