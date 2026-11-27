// Host display helpers for frame pacing. The game runs at 60 Hz and paces itself on the vblank; if the emulated
// vblank is a free-running timer and the monitor refreshes at something that is not a multiple of 60 (144 Hz),
// every image stays on screen for an irregular number of refreshes and 60 fps look choppy. These helpers let the
// vblank thread follow the real display: host_display_hz() / host_set_refresh() and host_wait_refresh().
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <cwchar>
#include <cmath>
#include <thread>
#include <chrono>
#include <utility>
#include <vector>
#include <initializer_list>
#include "recomp/host_ui.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <shlwapi.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
// kernel-mode-thunk vblank wait (gdi32): blocks until the next real vertical blank of the primary display
extern "C" {
struct KmtOpenAdapterFromHdc { HDC hDc; UINT hAdapter; LUID luid; UINT vidPnSourceId; };
struct KmtWaitVblank { UINT hAdapter; UINT hDevice; UINT vidPnSourceId; };
LONG __stdcall D3DKMTOpenAdapterFromHdc(KmtOpenAdapterFromHdc*);
LONG __stdcall D3DKMTWaitForVerticalBlankEvent(const KmtWaitVblank*);
}
namespace gti::recomp {
// refresh rate of the primary display in Hz (0 when unknown)
int host_display_hz() {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    return EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? int(dm.dmDisplayFrequency) : 0;
}
// Switches the primary display to `hz` at its current resolution (a dynamic change: nothing is saved, but it stays
// until host_restore_refresh() or the next mode change; the "temporary" CDS_FULLSCREEN flavour is accepted and then
// not applied by the compositor). False when the display has no such mode.
void host_restore_refresh();
static DEVMODEW g_savedMode{};
static bool g_modeChanged = false;
bool host_set_refresh(int hz) {
    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur)) return false;
    if (int(cur.dmDisplayFrequency) == hz) return true;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight || dm.dmBitsPerPel != cur.dmBitsPerPel || int(dm.dmDisplayFrequency) != hz) continue;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        if (ChangeDisplaySettingsExW(nullptr, &dm, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL) return false;
        g_savedMode = cur;
        g_savedMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
        g_modeChanged = true;
        // the display must go back to its mode however the process ends (crash, Ctrl+C, console closed)
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG { host_restore_refresh(); return EXCEPTION_CONTINUE_SEARCH; });
        SetConsoleCtrlHandler([](DWORD) -> BOOL { host_restore_refresh(); return FALSE; }, TRUE);
        std::atexit([] { host_restore_refresh(); });
        return true;
    }
    return false;
}
// puts the display back as host_set_refresh() found it
void host_restore_refresh() {
    if (!g_modeChanged) return;
    g_modeChanged = false;
    ChangeDisplaySettingsExW(nullptr, &g_savedMode, nullptr, 0, nullptr);
}
// blocks until the next vertical blank of the primary display; false when that is not available
bool host_wait_refresh() {
    static KmtWaitVblank w{};
    static int state = 0;   // 0 not opened, 1 ok, -1 unavailable
    if (state == 0) {
        // a DC of the primary monitor itself: the screen DC (GetDC(nullptr)) spans every monitor, and with two of
        // them the wait did not follow the one the game is on (frames took two refreshes or more)
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        const POINT origin{0, 0};
        GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
        KmtOpenAdapterFromHdc o{};
        o.hDc = CreateDCW(mi.szDevice, mi.szDevice, nullptr, nullptr);
        state = o.hDc && D3DKMTOpenAdapterFromHdc(&o) == 0 ? 1 : -1;
        if (o.hDc) DeleteDC(o.hDc);
        w.hAdapter = o.hAdapter; w.vidPnSourceId = o.vidPnSourceId;
    }
    if (state < 0) return false;
    if (D3DKMTWaitForVerticalBlankEvent(&w) != 0) { state = -1; return false; }
    return true;
}
// 1 ms Windows timer for the whole run. Short sleeps (the SPU job workers, the RSX thread and the vblank pacing
// all sleep tens of microseconds to a few milliseconds) otherwise last a whole 15.6 ms tick: on Windows 11 the
// resolution another program asked for no longer applies to this one, and frames halved to 30 per second.
void host_timer_init() {
    static const bool done = [] {
        HMODULE w = LoadLibraryW(L"winmm.dll");
        auto begin = w ? reinterpret_cast<UINT(WINAPI*)(UINT)>(reinterpret_cast<void*>(GetProcAddress(w, "timeBeginPeriod"))) : nullptr;
        // and no "efficiency mode" when the window is in the background (alt-tab to OBS): Windows would slow
        // the threads down (EcoQoS) and the game would stutter
        PROCESS_POWER_THROTTLING_STATE pt{};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;
        using SetInfo = BOOL(WINAPI*)(HANDLE, int, void*, DWORD);   // Windows 8+, looked up so older SDK headers do
        if (auto set = reinterpret_cast<SetInfo>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"))))
            set(GetCurrentProcess(), int(ProcessPowerThrottling), &pt, sizeof pt);
        return begin && begin(1) == 0;
    }();
    (void)done;
    // AFFINITY=hex mask (test): the game only gets those hardware threads, e.g. ff = a 4 core / 8 thread processor
    if (const char* m = std::getenv("AFFINITY")) SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(std::strtoull(m, nullptr, 16)));
    if (std::getenv("TIMERTEST")) {   // debug: how long short sleeps really take
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
        for (int us : {50, 500, 1000}) {
            QueryPerformanceCounter(&t0);
            for (int k = 0; k < 20; ++k) std::this_thread::sleep_for(std::chrono::microseconds(us));
            QueryPerformanceCounter(&t1);
            std::fprintf(stderr, "[timer] sleep %d us takes %.3f ms (timeBeginPeriod %s)%c", us, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 20, done ? "ok" : "failed", 10);
        }
    }
}
// time-critical work (sound mixing): the calling thread goes ahead of the normal ones
// also puts the whole process above normal programs once, like most games: OBS or a browser then cannot take its time slices
// Sleeps of well under a millisecond: a high-resolution waitable timer (Windows 10 1803+) wakes up about 0.5 ms
// late at most, where Sleep() rounds up to the timer period. Threads of the game that poll with short sleeps then
// really sleep instead of burning a core.
void host_sleep_us(uint64_t us) {
    static thread_local HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!t) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return; }
    LARGE_INTEGER due; due.QuadPart = -int64_t(us) * 10;
    if (!SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return; }
    WaitForSingleObject(t, INFINITE);
}
void host_thread_priority_high() {
    if (std::getenv("NOPRIO")) return;
    static const bool cls = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != 0; (void)cls;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}
// The RSX thread is the one every image waits for. With a dozen busy threads on 8 cores Windows sometimes put it on
// the second hardware thread of a core a geometry worker was using: it then ran a third slower for seconds on end
// (the start grid stuck at ~95 images per second instead of 144, run after run at random). own = true keeps the
// calling thread on the first physical core (logical CPUs 0 and 1); own = false keeps it off that core. Only on
// machines with at least 12 logical CPUs. AFINIDAD=0: off.
void host_thread_core(bool own) {
    // Off unless AFINIDAD is set: measured, no placement beat the scheduler (the slow runs were the test window
    // losing the focus, not the cores).
    static const bool off = !std::getenv("AFINIDAD") || std::getenv("AFINIDAD")[0] == '0';
    DWORD_PTR proc = 0, sys = 0;
    if (off || !GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return;
    int n = 0;
    for (DWORD_PTR m = proc; m; m >>= 1) n += int(m & 1);
    if (n < 12 || (proc & 3) != 3) return;
    // AFINIDAD=mode: 1 = own core is 0 (CPUs 0,1); 2 = own core is the last one; 3 = no pinning, only priority for the owner;
    // 4 = owner on one logical CPU of the last core, the others off that whole core
    static const int mode = std::getenv("AFINIDAD") ? std::atoi(std::getenv("AFINIDAD")) : 3;
    int hi = 0; for (int k = 0; k < 64; ++k) if (proc >> k & 1) hi = k;
    const DWORD_PTR last = DWORD_PTR(3) << (hi - 1);
    if (mode == 1) SetThreadAffinityMask(GetCurrentThread(), own ? DWORD_PTR(3) : (proc & ~DWORD_PTR(3)));
    else if (mode == 2) SetThreadAffinityMask(GetCurrentThread(), own ? last : (proc & ~last));
    else if (mode == 4) SetThreadAffinityMask(GetCurrentThread(), own ? (DWORD_PTR(1) << (hi - 1)) : (proc & ~last));
    if (own) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}
// "Aplicar cambios" in the options menu: starts the game again with the same command line and ends this process at
// once (the display mode goes back first, so the new process finds the desktop's own mode)
void host_restart() {
    host_restore_refresh();
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = GetCommandLineW();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 0);
}
// A small text box (options menu: online name, host address). Modal, on top of the game, Windows' own controls.
namespace {
struct InputBox { std::wstring prompt, value; int maxLen; };
INT_PTR CALLBACK input_box_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    InputBox* b = reinterpret_cast<InputBox*>(GetWindowLongPtrW(h, DWLP_USER));
    switch (m) {
        case WM_INITDIALOG:
            SetWindowLongPtrW(h, DWLP_USER, l); b = reinterpret_cast<InputBox*>(l);
            SetDlgItemTextW(h, 100, b->prompt.c_str());
            SetDlgItemTextW(h, 101, b->value.c_str());
            SendDlgItemMessageW(h, 101, EM_SETLIMITTEXT, WPARAM(b->maxLen), 0);
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(h);
            SetFocus(GetDlgItem(h, 101)); SendDlgItemMessageW(h, 101, EM_SETSEL, 0, -1);
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(w) == IDOK) { wchar_t t[256] = {}; GetDlgItemTextW(h, 101, t, 255); b->value = t; EndDialog(h, IDOK); return TRUE; }
            if (LOWORD(w) == IDCANCEL) { EndDialog(h, IDCANCEL); return TRUE; }
            break;
    }
    return FALSE;
}
void tpl_word(std::vector<WORD>& t, WORD v) { t.push_back(v); }
void tpl_str(std::vector<WORD>& t, const wchar_t* s) { for (; *s; ++s) t.push_back(WORD(*s)); t.push_back(0); }
void tpl_item(std::vector<WORD>& t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
    if (t.size() & 1) t.push_back(0);   // DLGITEMTEMPLATE on a DWORD boundary
    DLGITEMTEMPLATE it{}; it.style = style | WS_CHILD | WS_VISIBLE; it.x = x; it.y = y; it.cx = cx; it.cy = cy; it.id = id;
    const WORD* p = reinterpret_cast<const WORD*>(&it); t.insert(t.end(), p, p + sizeof it / 2);
    tpl_word(t, 0xFFFF); tpl_word(t, cls); tpl_str(t, text); tpl_word(t, 0);
}
} // namespace
bool host_input_text(const wchar_t* title, const wchar_t* prompt, std::string& value, int maxLen) {
    std::vector<WORD> t;
    DLGTEMPLATE d{}; d.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU; d.cdit = 4; d.cx = 240; d.cy = 74;
    const WORD* p = reinterpret_cast<const WORD*>(&d); t.insert(t.end(), p, p + sizeof d / 2);
    tpl_word(t, 0); tpl_word(t, 0); tpl_str(t, title);   // no menu, default class, caption
    tpl_word(t, 10); tpl_str(t, L"Segoe UI");            // DS_SETFONT
    tpl_item(t, SS_LEFT, 8, 7, 224, 20, 100, 0x0082, L"");
    tpl_item(t, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 30, 224, 13, 101, 0x0081, L"");
    tpl_item(t, BS_DEFPUSHBUTTON | WS_TABSTOP, 128, 52, 50, 15, IDOK, 0x0080, L"Aceptar");
    tpl_item(t, BS_PUSHBUTTON | WS_TABSTOP, 182, 52, 50, 15, IDCANCEL, 0x0080, L"Cancelar");
    InputBox b; b.prompt = prompt; b.maxLen = maxLen;
    for (unsigned char ch : value) b.value += wchar_t(ch);
    ShowCursor(TRUE);
    const INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(t.data()), GetForegroundWindow(), input_box_proc, LPARAM(&b));
    if (r != IDOK) return false;
    value.clear();
    for (wchar_t ch : b.value) if (ch >= 32 && ch < 127) value += char(ch);   // names and addresses: ASCII
    return true;
}
// "Informe de problemas" in the options menu: writes informe_problema.txt next to the executable (processor, memory,
// graphics card, settings and the end of the log, with the Windows user name taken out of the paths), shows it in
// Explorer and opens the project's "new issue" page, where the user can attach it.
void host_report(const char* gpu) {
    std::fflush(nullptr);
    auto slurp = [](const char* name, size_t tail) {
        std::string s;
        if (FILE* f = std::fopen(name, "rb")) {
            std::fseek(f, 0, SEEK_END); const long n = std::ftell(f);
            const long from = n > long(tail) ? n - long(tail) : 0;
            std::fseek(f, from, SEEK_SET);
            s.resize(size_t(n - from)); s.resize(std::fread(s.data(), 1, s.size(), f)); std::fclose(f);
        }
        return s;
    };
    char cpu[128] = "?"; DWORD cb = sizeof cpu;
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &cb);
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    SYSTEMTIME t; GetLocalTime(&t);
    char head[512];
    std::snprintf(head, sizeof head, "Racing Club+ - informe de problemas\r\nFecha: %04d-%02d-%02d %02d:%02d\r\nProcesador: %s (%u hilos)\r\nMemoria: %llu MB\r\nGrafica: %s\r\nPantalla: %d Hz\r\n\r\n--- gtiport.ini ---\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, cpu, std::thread::hardware_concurrency(), (unsigned long long)(ms.ullTotalPhys >> 20), gpu ? gpu : "?", host_display_hz());
    std::string out = head + slurp("gtiport.ini", 1 << 16) + "\r\n--- final de recomp_log.txt ---\r\n" + slurp("recomp_log.txt", 300000);
    char user[256]; DWORD un = sizeof user;
    if (GetUserNameA(user, &un) && un > 3) {   // paths in the log carry the Windows user name
        const std::string u = user;
        for (size_t p = 0; (p = out.find(u, p)) != std::string::npos;) out.replace(p, u.size(), "USUARIO");
    }
    wchar_t exe[1024]; std::wstring path = L"informe_problema.txt";
    if (const DWORD n = GetModuleFileNameW(nullptr, exe, 1024); n > 0 && n < 1024) { path.assign(exe, n); path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"informe_problema.txt"; }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    std::fwrite(out.data(), 1, out.size(), f); std::fclose(f);
    const std::wstring sel = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
    ShellExecuteW(nullptr, L"open", L"https://github.com/kensaiken/Racingclub/issues/new", nullptr, nullptr, SW_SHOWNORMAL);
}
// a trophy notification: dark rounded card, a medal of the trophy's colour (rgb 0xRRGGBB), title, name, detail
bool host_toast_bitmap(const wchar_t* title, const wchar_t* name, const wchar_t* detail, uint32_t rgb, const uint8_t* png, size_t pngSize, int w, int h, uint32_t* out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bm) { if (dc) DeleteDC(dc); return false; }
    HGDIOBJ oldBm = SelectObject(dc, bm);
    RECT all{0, 0, w, h};
    HBRUSH edge = CreateSolidBrush(RGB(0, 62, 190)), card = CreateSolidBrush(RGB(16, 24, 44));
    FillRect(dc, &all, edge);
    RECT in{3, 3, w - 3, h - 3};
    FillRect(dc, &in, card);
    // medal: outer ring + inner disc in the trophy colour
    const COLORREF mc = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    HBRUSH medal = CreateSolidBrush(mc), ring = CreateSolidBrush(RGB(255, 255, 255));
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ oldPen = SelectObject(dc, noPen);
    const int r = h * 32 / 100, cx = 18 + r, cy = h / 2;
    HGDIOBJ oldBr = SelectObject(dc, ring);
    bool icon = false;
    if (png && pngSize) {   // the trophy's own icon, framed in its colour
        static ULONG_PTR gdipToken = 0;
        if (!gdipToken) { Gdiplus::GdiplusStartupInput in; Gdiplus::GdiplusStartup(&gdipToken, &in, nullptr); }
        if (IStream* st = SHCreateMemStream(png, UINT(pngSize))) {
            Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(st);
            if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
                SelectObject(dc, medal);
                Rectangle(dc, cx - r - 3, cy - r - 3, cx + r + 3, cy + r + 3);
                Gdiplus::Graphics g(dc);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(bmp, cx - r, cy - r, 2 * r, 2 * r);
                icon = true;
            }
            delete bmp;
            st->Release();
        }
    }
    if (!icon) {
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, medal);
        Ellipse(dc, cx - r + 5, cy - r + 5, cx + r - 5, cy + r - 5);
    }
    SelectObject(dc, oldBr); SelectObject(dc, oldPen);
    SetBkMode(dc, TRANSPARENT);
    HFONT f1 = CreateFontW(-h * 17 / 100, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT f2 = CreateFontW(-h * 25 / 100, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT f3 = CreateFontW(-h * 15 / 100, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    const int x = cx + r + 16;
    RECT t1{x, h * 10 / 100, w - 12, h * 33 / 100}, t2{x, h * 33 / 100, w - 12, h * 63 / 100}, t3{x, h * 63 / 100, w - 12, h * 92 / 100};
    HGDIOBJ oldF = SelectObject(dc, f1);
    SetTextColor(dc, RGB(150, 190, 255));
    DrawTextW(dc, title, -1, &t1, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, f2);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, name, -1, &t2, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, f3);
    SetTextColor(dc, RGB(190, 198, 214));
    DrawTextW(dc, detail, -1, &t3, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldF);
    GdiFlush();
    memcpy(out, bits, size_t(w) * h * 4);
    {   // opaque card with rounded corners (the overlays are blended by their premultiplied alpha)
        const float rad = float(h) * 0.12f;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint32_t& p = out[size_t(y) * w + x];
                const float cx = x + 0.5f < rad ? rad : x + 0.5f > w - rad ? w - rad : x + 0.5f;
                const float cy = y + 0.5f < rad ? rad : y + 0.5f > h - rad ? h - rad : y + 0.5f;
                const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
                const float a = std::clamp(rad - d + 0.5f, 0.f, 1.f);
                if (a >= 1.f) { p |= 0xff000000u; continue; }
                const auto sc = [a](uint32_t c) { return uint32_t(float(c) * a + 0.5f); };
                p = (sc(255) << 24) | (sc((p >> 16) & 255) << 16) | (sc((p >> 8) & 255) << 8) | sc(p & 255);
            }
    }
    SelectObject(dc, oldBm);
    DeleteObject(f1); DeleteObject(f2); DeleteObject(f3); DeleteObject(edge); DeleteObject(card); DeleteObject(medal); DeleteObject(ring); DeleteObject(noPen);
    DeleteObject(bm); DeleteDC(dc);
    return true;
}
// the Patreon banner: a rounded card in Patreon's red with a white mark (bar + disc) and three lines of text
bool host_banner_bitmap(const wchar_t* line1, const wchar_t* line2, const wchar_t* line3, int w, int h, uint32_t* out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bm) { if (dc) DeleteDC(dc); return false; }
    HGDIOBJ oldBm = SelectObject(dc, bm);
    RECT all{0, 0, w, h};
    HBRUSH edge = CreateSolidBrush(RGB(255, 255, 255)), card = CreateSolidBrush(RGB(255, 66, 77)), white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, edge);
    RECT in{3, 3, w - 3, h - 3};
    FillRect(dc, &in, card);
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ oldPen = SelectObject(dc, noPen), oldBr = SelectObject(dc, white);
    const int r = h * 22 / 100, cy = h / 2, lx = 20, bw = r * 2 / 5;   // the mark: a bar and a disc
    Rectangle(dc, lx, cy - r - r / 3, lx + bw, cy + r + r / 3);
    Ellipse(dc, lx + bw + 6, cy - r - r / 3, lx + bw + 6 + 2 * r, cy - r - r / 3 + 2 * r);
    SelectObject(dc, oldBr); SelectObject(dc, oldPen);
    SetBkMode(dc, TRANSPARENT);
    HFONT f1 = CreateFontW(-h * 24 / 100, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT f2 = CreateFontW(-h * 17 / 100, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT f3 = CreateFontW(-h * 16 / 100, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    const int x = lx + bw + 6 + 2 * r + 18;
    RECT t1{x, h * 8 / 100, w - 12, h * 40 / 100}, t2{x, h * 40 / 100, w - 12, h * 66 / 100}, t3{x, h * 66 / 100, w - 12, h * 92 / 100};
    HGDIOBJ oldF = SelectObject(dc, f1);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, line1, -1, &t1, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, f2);
    SetTextColor(dc, RGB(255, 236, 238));
    DrawTextW(dc, line2, -1, &t2, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, f3);
    SetTextColor(dc, RGB(255, 255, 255));
    const std::wstring l3 = std::wstring(line3) + L"  \u2192";
    DrawTextW(dc, l3.c_str(), -1, &t3, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldF);
    GdiFlush();
    memcpy(out, bits, size_t(w) * h * 4);
    {   // opaque card with rounded corners (premultiplied alpha)
        const float rad = float(h) * 0.14f;
        for (int y = 0; y < h; ++y)
            for (int xx = 0; xx < w; ++xx) {
                uint32_t& p = out[size_t(y) * w + xx];
                const float cx = xx + 0.5f < rad ? rad : xx + 0.5f > w - rad ? w - rad : xx + 0.5f;
                const float cy2 = y + 0.5f < rad ? rad : y + 0.5f > h - rad ? h - rad : y + 0.5f;
                const float d = std::sqrt((xx + 0.5f - cx) * (xx + 0.5f - cx) + (y + 0.5f - cy2) * (y + 0.5f - cy2));
                const float a = std::clamp(rad - d + 0.5f, 0.f, 1.f);
                if (a >= 1.f) { p |= 0xff000000u; continue; }
                const auto sc = [a](uint32_t c) { return uint32_t(float(c) * a + 0.5f); };
                p = (sc(255) << 24) | (sc((p >> 16) & 255) << 16) | (sc((p >> 8) & 255) << 8) | sc(p & 255);
            }
    }
    SelectObject(dc, oldBm);
    DeleteObject(f1); DeleteObject(f2); DeleteObject(f3); DeleteObject(edge); DeleteObject(card); DeleteObject(white); DeleteObject(noPen);
    DeleteObject(bm); DeleteDC(dc);
    return true;
}
// ------------------------------------------------------------------ options panel (F1)
namespace {
void gdip_start() {
    static ULONG_PTR token = 0;
    if (!token) { Gdiplus::GdiplusStartupInput in; Gdiplus::GdiplusStartup(&token, &in, nullptr); }
}
using Gdiplus::Color; using Gdiplus::RectF; using Gdiplus::PointF;
void round_rect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = std::min(r, std::min(w, h) / 2);
    if (r <= 0.5f) { p.AddRectangle(RectF(x, y, w, h)); return; }
    const float d = 2 * r;
    p.AddArc(x, y, d, d, 180, 90); p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90); p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
void fill_round(Gdiplus::Graphics& g, const Gdiplus::Brush& b, float x, float y, float w, float h, float r) {
    Gdiplus::GraphicsPath p; round_rect(p, x, y, w, h, r); g.FillPath(&b, &p);
}
void stroke_round(Gdiplus::Graphics& g, Color c, float width, float x, float y, float w, float h, float r) {
    Gdiplus::GraphicsPath p; round_rect(p, x, y, w, h, r); Gdiplus::Pen pen(c, width); g.DrawPath(&pen, &p);
}
float text_w(Gdiplus::Graphics& g, const wchar_t* s, const Gdiplus::Font& f) {
    RectF r; g.MeasureString(s, -1, &f, PointF(0, 0), Gdiplus::StringFormat::GenericTypographic(), &r);
    return r.Width;
}
// one line of text, vertically centred on cy; align 0 left of x, 1 centred on x, 2 right of x
void text(Gdiplus::Graphics& g, const wchar_t* s, const Gdiplus::Font& f, Color c, float x, float cy, int align = 0) {
    Gdiplus::SolidBrush b(c);
    const float tw = text_w(g, s, f), h = f.GetHeight(&g);
    const float tx = align == 0 ? x : align == 1 ? x - tw / 2 : x - tw;
    g.DrawString(s, -1, &f, PointF(tx, cy - h / 2), Gdiplus::StringFormat::GenericTypographic(), &b);
}
const Color kAccent(255, 226, 38, 46), kAccent2(255, 255, 112, 40), kAmber(255, 255, 184, 64);
const Color kText(255, 244, 246, 250), kDim(255, 150, 160, 180), kFaint(255, 98, 108, 128);
// a key cap: a word ("Intro"), or a drawn controller symbol (1 cross, 2 circle, 3 d-pad)
float keycap(Gdiplus::Graphics& g, const wchar_t* s, int symbol, const Gdiplus::Font& f, float x, float cy, float hgt) {
    const float pad = hgt * 0.32f;
    const float w = symbol ? hgt : std::max(hgt, text_w(g, s, f) + 2 * pad);
    Gdiplus::SolidBrush bg(Color(40, 255, 255, 255));
    fill_round(g, bg, x, cy - hgt / 2, w, hgt, hgt * 0.22f);
    stroke_round(g, Color(70, 255, 255, 255), 1.f, x, cy - hgt / 2, w, hgt, hgt * 0.22f);
    const float cx = x + w / 2, r = hgt * 0.26f;
    Gdiplus::Pen pen(kText, std::max(1.5f, hgt * 0.09f));
    pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
    if (symbol == 1) { g.DrawLine(&pen, cx - r, cy - r, cx + r, cy + r); g.DrawLine(&pen, cx - r, cy + r, cx + r, cy - r); }
    else if (symbol == 2) g.DrawEllipse(&pen, cx - r, cy - r, 2 * r, 2 * r);
    else if (symbol == 3) {
        Gdiplus::SolidBrush b(kText);
        const float a = r * 0.42f, l = r * 1.15f;
        g.FillRectangle(&b, cx - a, cy - l, 2 * a, 2 * l); g.FillRectangle(&b, cx - l, cy - a, 2 * l, 2 * a);
    } else text(g, s, f, kText, cx, cy, 1);
    return w;
}
}

// A button icon like the game's PlayStation ones (dark disc, coloured symbol) for another controller: style 2 Xbox
// (A green, B red, X blue, Y yellow), 3 Nintendo (white letters); symbol = the PlayStation button at that place
// (0 cross = bottom, 1 circle = right, 2 square = left, 3 triangle = top). w x h straight-alpha RGBA.
bool host_button_glyph(int style, int symbol, int w, int h, uint8_t* rgba) {
    if (style != 2 && style != 3) return false;
    gdip_start();
    const int S = 4, W = w * S, H = h * S;
    std::vector<uint32_t> big(size_t(W) * H, 0);
    {
        Gdiplus::Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(big.data()));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        const float d = float(std::min(W, H)) - 1.f, x = (W - d) / 2, y = (H - d) / 2;
        Gdiplus::SolidBrush rim(Color(255, 8, 8, 10));
        g.FillEllipse(&rim, x, y, d, d);
        const float in = 1.3f * S;
        Gdiplus::LinearGradientBrush fill(PointF(0, y), PointF(0, y + d), Color(255, 84, 86, 92), Color(255, 30, 31, 35));
        g.FillEllipse(&fill, x + in, y + in, d - 2 * in, d - 2 * in);
        static const wchar_t* const xbox[4] = {L"A", L"B", L"X", L"Y"};
        static const wchar_t* const nin[4] = {L"B", L"A", L"Y", L"X"};
        static const Color xcol[4] = {Color(255, 110, 205, 60), Color(255, 245, 70, 60), Color(255, 70, 150, 255), Color(255, 250, 205, 40)};
        const wchar_t* t = style == 2 ? xbox[symbol & 3] : nin[symbol & 3];
        const Color c = style == 2 ? xcol[symbol & 3] : Color(255, 245, 245, 245);
        Gdiplus::FontFamily fam(L"Segoe UI");
        Gdiplus::Font f(&fam, d * 0.68f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        text(g, t, f, c, W / 2.f, H / 2.f + d * 0.01f, 1);
        g.Flush(Gdiplus::FlushIntentionSync);
    }
    for (int yy = 0; yy < h; ++yy)   // box filter down to w x h, premultiplied BGRA -> straight RGBA
        for (int xx = 0; xx < w; ++xx) {
            uint32_t sb = 0, sg = 0, sr = 0, sa = 0;
            for (int j = 0; j < S; ++j)
                for (int i = 0; i < S; ++i) {
                    const uint32_t p = big[size_t(yy * S + j) * W + xx * S + i];
                    sb += p & 255; sg += (p >> 8) & 255; sr += (p >> 16) & 255; sa += p >> 24;
                }
            uint8_t* o = rgba + (size_t(yy) * w + xx) * 4;
            const uint32_t a = sa / (S * S);
            o[3] = uint8_t(a);
            o[0] = uint8_t(a ? std::min<uint32_t>(255, sr * 255 / sa) : 0);
            o[1] = uint8_t(a ? std::min<uint32_t>(255, sg * 255 / sa) : 0);
            o[2] = uint8_t(a ? std::min<uint32_t>(255, sb * 255 / sa) : 0);
        }
    return true;
}

bool host_menu_bitmap(const MenuPage& pg, int w, int h, uint32_t* out) {
    if (w < 64 || h < 64) return false;
    gdip_start();
    std::memset(out, 0, size_t(w) * h * 4);
    Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(out));
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float u = float(h) / 100.f;                // layout unit: 1 % of the panel height
    const float m = 1.6f * u;                         // margin for the shadow
    const float X = m, Y = m, W = float(w) - 2 * m, H = float(h) - 2 * m, R = 2.2f * u;
    const float px = X + 4.2f * u, pr = X + W - 4.2f * u;   // content left and right
    Gdiplus::FontFamily fam(L"Segoe UI");
    Gdiplus::Font fBrand(&fam, 4.6f * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fSub(&fam, 2.1f * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fTab(&fam, 2.7f * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fRow(&fam, 3.0f * u, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fVal(&fam, 2.8f * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fSmall(&fam, 2.2f * u, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fBadge(&fam, 1.55f * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fKey(&fam, 1.9f * u, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

    // shadow, panel, border
    for (int k = 6; k >= 1; --k) {
        Gdiplus::SolidBrush s(Color(BYTE(14), 0, 0, 0));
        const float e = k * m / 6;
        fill_round(g, s, X - e, Y - e + m * 0.3f, W + 2 * e, H + 2 * e, R + e);
    }
    {
        Gdiplus::LinearGradientBrush bg(PointF(0, Y), PointF(0, Y + H), Color(246, 20, 25, 38), Color(250, 9, 11, 18));
        fill_round(g, bg, X, Y, W, H, R);
        stroke_round(g, Color(36, 255, 255, 255), 1.f, X + 0.5f, Y + 0.5f, W - 1, H - 1, R);
    }
    // header: name, section, status; then a red-orange stripe
    const float hy = Y + 7.2f * u;
    {
        text(g, L"RACING CLUB", fBrand, kText, px, hy);
        const float bw = text_w(g, L"RACING CLUB", fBrand);
        text(g, L"+", fBrand, kAccent, px + bw + 0.4f * u, hy);
        const float sx = px + bw + 0.4f * u + text_w(g, L"+", fBrand) + 2.2f * u;
        Gdiplus::SolidBrush sep(Color(60, 255, 255, 255));
        g.FillRectangle(&sep, sx, hy - 2.2f * u, std::max(1.f, 0.18f * u), 4.4f * u);
        text(g, L"OPCIONES", fSub, kDim, sx + 2.0f * u, hy);
        text(g, pg.status, fSmall, kFaint, pr, hy, 2);
    }
    const float sy = Y + 13.4f * u;
    {
        Gdiplus::LinearGradientBrush st(PointF(X, 0), PointF(X + W, 0), kAccent, Color(0, 255, 112, 40));
        Gdiplus::Color cs[3] = {kAccent, kAccent2, Color(0, 255, 112, 40)}; Gdiplus::REAL ps[3] = {0.f, 0.45f, 1.f};
        st.SetInterpolationColors(cs, ps, 3);
        g.FillRectangle(&st, X, sy, W, 0.45f * u);
    }
    // tabs
    const float ty = Y + 19.2f * u;
    {
        const float capH = 3.4f * u;
        float x = px;
        if (pg.nTabs > 1) x += (pg.pad ? keycap(g, L"L1", 0, fKey, x, ty, capH) : keycap(g, L"Q", 0, fKey, x, ty, capH)) + 2.4f * u;
        for (int t = 0; t < pg.nTabs; ++t) {
            const float tw = text_w(g, pg.tabs[t], fTab);
            if (t == pg.tab) {
                Gdiplus::SolidBrush on(Color(26, 255, 255, 255));
                fill_round(g, on, x - 1.8f * u, ty - 2.8f * u, tw + 3.6f * u, 5.6f * u, 1.0f * u);
                Gdiplus::SolidBrush ul(kAccent);
                fill_round(g, ul, x - 0.6f * u, ty + 2.8f * u - 0.5f * u, tw + 1.2f * u, 0.5f * u, 0.25f * u);
            }
            text(g, pg.tabs[t], fTab, t == pg.tab ? kText : kDim, x, ty);
            x += tw + 5.0f * u;
        }
        if (pg.nTabs > 1) { x += -2.6f * u; pg.pad ? keycap(g, L"R1", 0, fKey, x, ty, capH) : keycap(g, L"E", 0, fKey, x, ty, capH); }
    }
    // rows
    const float rowH = 8.4f * u, r0 = Y + 26.0f * u;
    for (int i = 0; i < pg.nRows; ++i) {
        const MenuRow& r = pg.rows[i];
        const float top = r0 + i * rowH, cy = top + rowH / 2;
        const bool sel = i == pg.sel;
        if (sel) {
            Gdiplus::LinearGradientBrush hb(PointF(X + 2.0f * u, 0), PointF(X + W - 2.0f * u, 0), Color(235, 206, 32, 44), Color(170, 120, 20, 34));
            fill_round(g, hb, X + 2.0f * u, top + 0.5f * u, W - 4.0f * u, rowH - 1.0f * u, 1.3f * u);
            Gdiplus::SolidBrush bar(Color(255, 255, 255, 255));
            fill_round(g, bar, X + 2.0f * u, top + 2.1f * u, 0.5f * u, rowH - 4.2f * u, 0.25f * u);
        } else if (i > 0) {
            Gdiplus::SolidBrush sep(Color(16, 255, 255, 255));
            g.FillRectangle(&sep, px, top, pr - px, std::max(1.f, 0.12f * u));
        }
        // label + "reinicio" badge (amber dot when it is waiting for the restart)
        text(g, r.label, fRow, sel ? Color(255, 255, 255, 255) : kText, px, cy);
        if (r.restart) {
            const float bx = px + text_w(g, r.label, fRow) + 1.6f * u, bw = text_w(g, L"REINICIO", fBadge) + 1.8f * u, bh = 2.4f * u;
            const Color bc = sel ? Color(220, 255, 255, 255) : (r.pending ? kAmber : Color(255, 120, 130, 150));
            stroke_round(g, bc, std::max(1.f, 0.14f * u), bx, cy - bh / 2, bw, bh, bh / 2);
            if (r.pending) { Gdiplus::SolidBrush d(bc); fill_round(g, d, bx, cy - bh / 2, bw, bh, bh / 2); }
            text(g, L"REINICIO", fBadge, r.pending ? Color(255, 24, 18, 10) : bc, bx + bw / 2, cy, 1);
        }
        // value
        if (r.kind == MenuRow::Toggle) {
            const float sw = 6.6f * u, sh = 3.4f * u, sx = pr - sw;
            Gdiplus::SolidBrush track(r.on ? (sel ? Color(255, 255, 255, 255) : kAccent) : Color(70, 255, 255, 255));
            fill_round(g, track, sx, cy - sh / 2, sw, sh, sh / 2);
            const float kr = sh / 2 - 0.45f * u, kx = r.on ? sx + sw - sh / 2 : sx + sh / 2;
            Gdiplus::SolidBrush knob(r.on && sel ? kAccent : Color(255, 250, 250, 252));
            g.FillEllipse(&knob, kx - kr, cy - kr, 2 * kr, 2 * kr);
            text(g, r.on ? L"Sí" : L"No", fVal, sel ? kText : (r.on ? kText : kDim), sx - 1.6f * u, cy, 2);
        } else if (r.kind == MenuRow::Action) {
            const float bw = text_w(g, r.value, fVal) + 4.4f * u, bh = 5.0f * u, bx = pr - bw;
            if (sel) { Gdiplus::SolidBrush b(Color(255, 255, 255, 255)); fill_round(g, b, bx, cy - bh / 2, bw, bh, 1.0f * u); }
            else stroke_round(g, Color(120, 255, 255, 255), std::max(1.f, 0.15f * u), bx, cy - bh / 2, bw, bh, 1.0f * u);
            text(g, r.value, fVal, sel ? Color(255, 190, 24, 36) : kText, bx + bw / 2, cy, 1);
        } else {
            const float vw = text_w(g, r.value, fVal), aw = 2.4f * u;
            const float vr = pr - aw, vx = vr - vw;
            const Color ac = sel ? Color(255, 255, 255, 255) : Color(120, 255, 255, 255);
            text(g, L"›", fVal, ac, pr, cy - 0.25f * u, 2);
            text(g, r.value, fVal, kText, vr - 0.8f * u, cy, 2);
            text(g, L"‹", fVal, ac, vx - 1.6f * u, cy - 0.25f * u, 2);
            if (r.note && *r.note) text(g, r.note, fSmall, sel ? Color(220, 255, 255, 255) : kAmber, vx - 4.2f * u, cy, 2);
        }
    }
    // description of the highlighted option
    const float dy = Y + 70.8f * u;
    {
        Gdiplus::SolidBrush box(Color(20, 255, 255, 255));
        fill_round(g, box, px - 1.0f * u, dy, pr - px + 2.0f * u, 9.6f * u, 1.2f * u);
        const float ic = 1.5f * u, icx = px + 1.6f * u, icy = dy + 4.8f * u;
        Gdiplus::SolidBrush ib(Color(255, 120, 170, 255));
        g.FillEllipse(&ib, icx - ic, icy - ic, 2 * ic, 2 * ic);
        text(g, L"i", fKey, Color(255, 14, 18, 30), icx, icy, 1);
        Gdiplus::SolidBrush tb(Color(255, 196, 204, 220));
        Gdiplus::StringFormat sf; sf.SetLineAlignment(Gdiplus::StringAlignmentCenter); sf.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
        g.DrawString(pg.help, -1, &fSmall, RectF(icx + ic + 1.6f * u, dy + 0.4f * u, pr - (icx + ic + 1.6f * u), 8.8f * u), &sf, &tb);
    }
    // banner: something waits for the restart
    const float by = Y + 82.6f * u;
    if (pg.pending) {
        Gdiplus::SolidBrush b(Color(40, 255, 184, 64));
        fill_round(g, b, px - 1.0f * u, by, pr - px + 2.0f * u, 4.6f * u, 1.0f * u);
        Gdiplus::SolidBrush d(kAmber);
        g.FillEllipse(&d, px + 0.6f * u, by + 1.6f * u, 1.4f * u, 1.4f * u);
        text(g, L"Hay cambios que se aplican al reiniciar:  Sistema  ›  Aplicar cambios", fSmall, kAmber, px + 3.0f * u, by + 2.3f * u);
    }
    // controls
    {
        const float fy = Y + H - 5.0f * u, capH = 3.4f * u, gap = 0.6f * u, after = 3.0f * u;
        Gdiplus::SolidBrush line(Color(22, 255, 255, 255));
        g.FillRectangle(&line, X + 2.0f * u, fy - 3.6f * u, W - 4.0f * u, std::max(1.f, 0.12f * u));
        float x = px;
        auto hint = [&](std::initializer_list<std::pair<const wchar_t*, int>> caps, const wchar_t* what) {
            for (const auto& c : caps) x += keycap(g, c.first, c.second, fKey, x, fy, capH) + gap;
            x += 0.6f * u;
            text(g, what, fSmall, kDim, x, fy);
            x += text_w(g, what, fSmall) + after;
        };
        if (pg.pad) {
            hint({{L"", 3}}, L"Elegir y cambiar");
            hint({{L"L1", 0}, {L"R1", 0}}, L"Pestaña");
            hint({{L"", 1}}, L"Aceptar");
            hint({{L"", 2}}, L"Cerrar");
        } else {
            hint({{L"↑", 0}, {L"↓", 0}}, L"Elegir");
            hint({{L"←", 0}, {L"→", 0}}, L"Cambiar");
            hint({{L"Q", 0}, {L"E", 0}}, L"Pestaña");
            hint({{L"Intro", 0}}, L"Aceptar");
            hint({{L"F1", 0}, {L"Esc", 0}}, L"Cerrar");
        }
    }
    g.Flush(Gdiplus::FlushIntentionSync);
    return true;
}
} // namespace gti::recomp
#else
namespace gti::recomp {
int host_display_hz() { return 0; }
bool host_set_refresh(int) { return false; }
void host_restore_refresh() {}
bool host_wait_refresh() { return false; }
bool host_menu_bitmap(const MenuPage&, int, int, uint32_t*) { return false; }
bool host_button_glyph(int, int, int, int, uint8_t*) { return false; }
void host_restart() {}
void host_report(const char*) {}
bool host_toast_bitmap(const wchar_t*, const wchar_t*, const wchar_t*, uint32_t, const uint8_t*, size_t, int, int, uint32_t*) { return false; }
bool host_banner_bitmap(const wchar_t*, const wchar_t*, const wchar_t*, int, int, uint32_t*) { return false; }
void host_sleep_us(uint64_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void host_thread_priority_high() {}
void host_thread_core(bool) {}
void host_timer_init() {}
}
#endif
