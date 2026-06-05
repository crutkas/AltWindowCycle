// AltWindowCycle (C++ POC)
// macOS Cmd+` behavior on Windows: cycle top-level windows of the *foreground app*.
//   Alt+`        -> next window of same app
//   Shift+Alt+`  -> previous window
//
// Two interaction modes (toggle with --no-overlay):
//   * Overlay (default): tap-and-hold Alt to bring up an Alt-Tab-style picker with
//     acrylic-backed masked preview snapshots; each tap of ` advances the selection;
//     release Alt to commit, Esc to cancel. A quick tap+release never shows the
//     overlay (instant).
//   * Instant (--no-overlay): the original behavior, switches immediately.
//
// Tiny, dependency-free Win32 (Win32 + DWM/GDI/GDI+). No CRT UI deps.
// The overlay/cycle logic is wrapped in a single `Switcher` class so it ports
// directly into the in-proc PowerToys C++ module.
// Build: see build.ps1 (cl /O1 /Os /MT, /SUBSYSTEM:WINDOWS).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <objidl.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <cwchar>
#include <cstdlib>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "gdiplus.lib")

// Win11 system backdrop (acrylic) attributes; defined here in case the SDK is older.
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3 // Desktop Acrylic, for transient surfaces (flyouts, Alt-Tab)
#endif

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// Undocumented but stable composition API used by the shell (and Alt-Tab) to give
// a window a blurred "acrylic" backdrop. Works on layered/region-clipped popups,
// unlike DWMWA_SYSTEMBACKDROP_TYPE which needs a normal framed window.
enum ACCENT_STATE
{
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};
struct ACCENT_POLICY
{
    DWORD AccentState;
    DWORD AccentFlags;
    DWORD GradientColor; // 0xAABBGGRR — tint colour + opacity over the blur
    DWORD AnimationId;
};
struct WINDOWCOMPOSITIONATTRIBDATA
{
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
};
#define WCA_ACCENT_POLICY 19
typedef BOOL(WINAPI* PFN_SetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

namespace AltTabStyle
{
    constexpr DWORD DefaultAcrylicGradientABGR = 0x68787880;

    inline COLORREF DebugMagentaRef() { return RGB(255, 0, 255); }
    inline COLORREF AccentFallbackRef() { return RGB(0, 120, 215); }
    inline COLORREF HeaderTextRef(bool selected)
    {
        // Approximate WinUI dark TextFillColorPrimary/Secondary as neutral opaque
        // grays; keep these RGB channels equal to avoid color tint in diagnostics.
        return selected ? RGB(255, 255, 255) : RGB(207, 207, 207);
    }

    inline Gdiplus::Color Transparent() { return Gdiplus::Color(0, 0, 0, 0); }
    inline Gdiplus::Color TranslucentBackdrop() { return Gdiplus::Color(180, 0, 255, 0); }
    inline Gdiplus::Color DebugMagenta() { return Gdiplus::Color(255, 255, 0, 255); }
    inline Gdiplus::Color WhitePreview() { return Gdiplus::Color(255, 255, 255, 255); }
    inline Gdiplus::Color Card(bool selected)
    {
        return selected ? Gdiplus::Color(255, 58, 58, 62)
                        : Gdiplus::Color(255, 43, 43, 46);
    }
    inline Gdiplus::Color PreviewMask(bool selected, bool magentaDebug, bool translucentBackdrop)
    {
        if (translucentBackdrop)
            return Gdiplus::Color(255, 0, 255, 0);
        if (magentaDebug)
            return DebugMagenta();
        return Card(selected);
    }
    inline Gdiplus::Color Accent(COLORREF accent)
    {
        return Gdiplus::Color(255, GetRValue(accent), GetGValue(accent), GetBValue(accent));
    }
    inline Gdiplus::Color FocusShadow() { return Gdiplus::Color(150, 0, 0, 0); }
}

static DWORD GetAcrylicGradientABGR()
{
    wchar_t value[32] = {};
    DWORD len = GetEnvironmentVariableW(L"AWC_ACRYLIC_ABGR", value, ARRAYSIZE(value));
    if (len > 0 && len < ARRAYSIZE(value))
    {
        wchar_t* end = nullptr;
        DWORD parsed = static_cast<DWORD>(wcstoul(value, &end, 0));
        if (end && *end == L'\0')
            return parsed;
    }
    return AltTabStyle::DefaultAcrylicGradientABGR;
}

// Give `hwnd` a dark acrylic backdrop (blurred desktop + dark tint), like Alt-Tab.
static void EnableAcrylic(HWND hwnd, DWORD gradientColorABGR)
{
    static PFN_SetWindowCompositionAttribute fn =
        reinterpret_cast<PFN_SetWindowCompositionAttribute>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                           "SetWindowCompositionAttribute"));
    if (!fn)
        return;
    ACCENT_POLICY accent = {};
    accent.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    accent.AccentFlags = 0;
    accent.GradientColor = gradientColorABGR;
    WINDOWCOMPOSITIONATTRIBDATA data = {};
    data.Attrib = WCA_ACCENT_POLICY;
    data.pvData = &accent;
    data.cbData = sizeof(accent);
    fn(hwnd, &data);
}


static const int HK_FORWARD = 1;
static const int HK_BACKWARD = 2;

static HICON GetWindowIcon(HWND hwnd); // defined below; used by ShowOverlayWindow

struct SnapshotThumb
{
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;
};

// =================== Window enumeration / activation =========================

// Full image path of a window's owning process. Empty on failure.
static std::wstring ProcessImagePath(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid)
        return std::wstring();

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return std::wstring();

    wchar_t buf[MAX_PATH * 2];
    DWORD len = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, buf, &len))
        result.assign(buf, len);
    CloseHandle(proc);
    return result;
}

// Standard "does this window appear in Alt+Tab" test.
static bool IsAltTabWindow(HWND hwnd)
{
    if (!IsWindowVisible(hwnd))
        return false;

    // Skip cloaked windows (e.g. background UWP / virtual-desktop hidden).
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    // Walk to the root owner, then to its last visible active popup.
    HWND walk = GetAncestor(hwnd, GA_ROOTOWNER);
    HWND tryPopup = nullptr;
    for (;;)
    {
        tryPopup = GetLastActivePopup(walk);
        if (tryPopup == walk)
            break;
        if (IsWindowVisible(tryPopup))
            break;
        walk = tryPopup;
    }
    if (walk != hwnd)
        return false;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW)
        return false;

    return true;
}

struct EnumCtx
{
    std::wstring exe;
    std::vector<HWND> windows;
};

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
{
    EnumCtx* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (!IsAltTabWindow(hwnd))
        return TRUE;
    std::wstring exe = ProcessImagePath(hwnd);
    if (!exe.empty() && _wcsicmp(exe.c_str(), ctx->exe.c_str()) == 0)
        ctx->windows.push_back(hwnd);
    return TRUE;
}

// Collect the Alt-Tab-eligible windows of the foreground app, in a stable order.
// Returns false if there is no usable foreground app.
static bool GetAppWindows(HWND& foreground, std::vector<HWND>& windows)
{
    windows.clear();
    foreground = GetForegroundWindow();
    if (!foreground)
        return false;

    EnumCtx ctx;
    ctx.exe = ProcessImagePath(foreground);
    if (ctx.exe.empty())
        return false;

    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));

    // Preserve EnumWindows Z-order, which is the system's most-recently-used
    // order — so cycling matches real Alt-Tab (most recent first). The list is
    // snapshotted once per hold, so the order stays stable while cycling.
    windows = std::move(ctx.windows);
    return !windows.empty();
}

// Reliably bring a window to the foreground.
static void ForceForeground(HWND hwnd)
{
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD myThread = GetCurrentThreadId();

    if (fgThread && fgThread != myThread)
        AttachThreadInput(myThread, fgThread, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (fgThread && fgThread != myThread)
        AttachThreadInput(myThread, fgThread, FALSE);
}

// =============================== Switcher ====================================

class Switcher
{
public:
    bool showOverlay = true;

    bool Init(HINSTANCE instance);
    void Shutdown();

    // Alt+` (forward) or Shift+Alt+` (backward) was pressed.
    void OnHotkey(bool forward);

    // Diagnostics: force-show the overlay for the app with the most windows and
    // keep it up for a few seconds so the render path can be verified headlessly.
    void RunSelfTest();

private:
    enum class St { Idle, Pending, Visible };

    static const UINT_PTR TIMER_ID = 1;
    static const UINT TIMER_MS = 25;
    static const DWORD SHOW_DELAY_MS = 180; // hold longer than this to reveal the overlay
    static const int MAX_COLS = 6;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void OnTick();
    void Commit();
    void Cancel();

    void ShowOverlayWindow();
    void HideOverlayWindow();
    void SetSelection(int index);
    void RenderBackdrop();
    void RenderLayered();
    void ComputeLayout(const RECT& work, int& x, int& y, int& panelW, int& panelH);
    void CaptureSnapshots();
    void ClearSnapshots();
    void RegisterThumbnails();
    void UnregisterThumbnails();
    void EnsureFont();

    RECT TileRect(int index) const;
    RECT PreviewRect(const RECT& tile) const;
    RECT HeaderRect(const RECT& tile) const;

    static int Wrap(int i, int n) { return ((i % n) + n) % n; }
    int Scaled(int v) const { return static_cast<int>(v * scale + 0.5); }

    HINSTANCE hinst = nullptr;
    HWND backdropHost = nullptr; // optional translucent rounded background behind thumbnails
    HWND overlay = nullptr;     // layered chrome (cards/header/ring) with preview holes
    HWND thumbHost = nullptr;   // host BEHIND overlay; acrylic backdrop + DWM thumbnails
    int ovX = 0, ovY = 0, ovW = 0, ovH = 0; // overlay screen rect (for UpdateLayeredWindow)

    St state = St::Idle;
    std::vector<HWND> windows;
    std::vector<HTHUMBNAIL> thumbs; // live DWM thumbnails (instant), rounded by overlay mask
    std::vector<SnapshotThumb> snapshots; // same-surface thumbnails for true alpha masking
    std::vector<HICON> icons;
    HWND anchorWindow = nullptr; // foreground window that invoked the current cycle
    int selected = 0;
    DWORD startTick = 0;

    // Layout (physical pixels, DPI-scaled at show time).
    double scale = 1.0;
    int cols = 1, rows = 1;
    int pad = 0, gap = 0, tileW = 0, tileH = 0, previewH = 0, inner = 0, radius = 0;
    int cardTrimBottom = 0;
    int headerH = 0, iconSize = 0;

    HFONT font = nullptr;
    double fontScale = 0.0;
    bool snapshotThumbnails = false;
};

// ---- lifecycle --------------------------------------------------------------

bool Switcher::Init(HINSTANCE instance)
{
    hinst = instance;

    // Host class for the DWM-thumbnail window that sits BEHIND the layered overlay.
    // It carries the dark acrylic backdrop (blur shows in the gaps between cards) and
    // is clipped to the rounded panel shape. Background brush is null so the acrylic
    // blur is not erased (or magenta in the AWC_MAGENTA debug build so any uncovered
    // preview pixel is glaringly obvious).
    WNDCLASSW hc = {};
    hc.style = CS_HREDRAW | CS_VREDRAW;
    hc.lpfnWndProc = &DefWindowProcW;
    hc.hInstance = hinst;
    hc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hc.hbrBackground = GetEnvironmentVariableW(L"AWC_MAGENTA", nullptr, 0)
                           ? CreateSolidBrush(AltTabStyle::DebugMagentaRef())
                           : nullptr;
    hc.lpszClassName = L"AltWindowCycleThumbHost";
    RegisterClassW(&hc);

    thumbHost = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        hc.lpszClassName, L"", WS_POPUP | WS_DISABLED,
        0, 0, 0, 0, nullptr, nullptr, hinst, nullptr);
    if (!thumbHost)
        return false;

    WNDCLASSW bc = {};
    bc.lpfnWndProc = &DefWindowProcW;
    bc.hInstance = hinst;
    bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    bc.hbrBackground = nullptr;
    bc.lpszClassName = L"AltWindowCycleBackdropHost";
    RegisterClassW(&bc);

    backdropHost = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        bc.lpszClassName, L"", WS_POPUP | WS_DISABLED,
        0, 0, 0, 0, nullptr, nullptr, hinst, nullptr);
    if (!backdropHost)
        return false;

    WNDCLASSW wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Switcher::WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"AltWindowCycleOverlay";
    RegisterClassW(&wc);

    // Per-pixel-alpha LAYERED popup; content pushed via UpdateLayeredWindow. The
    // preview areas are punched transparent so the thumbHost behind shows through.
    overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        wc.lpszClassName, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hinst, nullptr);
    if (!overlay)
        return false;

    SetWindowLongPtrW(overlay, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

void Switcher::Shutdown()
{
    UnregisterThumbnails();
    ClearSnapshots();
    if (thumbHost)
    {
        DestroyWindow(thumbHost);
        thumbHost = nullptr;
    }
    if (backdropHost)
    {
        DestroyWindow(backdropHost);
        backdropHost = nullptr;
    }
    if (font)
    {
        DeleteObject(font);
        font = nullptr;
    }
    if (overlay)
    {
        KillTimer(overlay, TIMER_ID);
        DestroyWindow(overlay);
        overlay = nullptr;
    }
}

// ---- state machine ----------------------------------------------------------

void Switcher::OnHotkey(bool forward)
{
    if (!showOverlay)
    {
        // Original instant-switch behavior.
        HWND fg;
        std::vector<HWND> list;
        if (!GetAppWindows(fg, list) || list.size() < 2)
            return;
        int idx = -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == fg) { idx = static_cast<int>(i); break; }
        int n = static_cast<int>(list.size());
        int target = (idx < 0) ? 0 : Wrap(forward ? idx + 1 : idx - 1, n);
        ForceForeground(list[target]);
        return;
    }

    if (state == St::Idle)
    {
        HWND fg;
        if (!GetAppWindows(fg, windows) || windows.size() < 2)
            return;

        int idx = -1;
        for (size_t i = 0; i < windows.size(); ++i)
            if (windows[i] == fg) { idx = static_cast<int>(i); break; }
        if (idx < 0)
            idx = 0;

        anchorWindow = fg;
        selected = Wrap(forward ? idx + 1 : idx - 1, static_cast<int>(windows.size()));
        state = St::Pending;
        startTick = GetTickCount();
        SetTimer(overlay, TIMER_ID, TIMER_MS, nullptr);
    }
    else
    {
        selected = Wrap(forward ? selected + 1 : selected - 1, static_cast<int>(windows.size()));
        if (state == St::Visible)
            SetSelection(selected);
    }
}

void Switcher::OnTick()
{
    bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

    if (escDown)
    {
        Cancel();
        return;
    }
    if (!altDown)
    {
        Commit();
        return;
    }
    if (state == St::Pending && (GetTickCount() - startTick) >= SHOW_DELAY_MS)
    {
        ShowOverlayWindow();
        state = St::Visible;
    }
}

void Switcher::Commit()
{
    KillTimer(overlay, TIMER_ID);
    if (state == St::Visible)
        HideOverlayWindow();
    anchorWindow = nullptr;

    if (selected >= 0 && selected < static_cast<int>(windows.size()))
    {
        HWND target = windows[selected];
        if (IsWindow(target))
            ForceForeground(target);
    }
    state = St::Idle;
}

void Switcher::Cancel()
{
    KillTimer(overlay, TIMER_ID);
    if (state == St::Visible)
        HideOverlayWindow();
    anchorWindow = nullptr;
    state = St::Idle;
}

// ---- overlay window ---------------------------------------------------------

static HMONITOR GetAnchorMonitor(HWND anchor)
{
    return MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
}

static UINT GetMonitorEffectiveDpi(HMONITOR mon)
{
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX)
        return dpiX;
    return 96;
}

static RECT GetWorkArea(HMONITOR mon)
{
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi))
        return mi.rcWork;
    RECT fallback = { 0, 0, 1920, 1080 };
    return fallback;
}

static UINT GetDpiForHostOnMonitor(HWND host, HMONITOR mon, const RECT& work)
{
    if (host && mon)
    {
        // Move our own per-monitor-v2 popup onto the target monitor first. Then
        // GetDpiForWindow reports the DPI Windows will actually use for this overlay,
        // independent of the target app's DPI awareness.
        SetWindowPos(host, nullptr, work.left, work.top, 1, 1,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_HIDEWINDOW);
        UINT dpi = GetDpiForWindow(host);
        if (dpi)
            return dpi;
    }
    return GetMonitorEffectiveDpi(mon);
}

void Switcher::ShowOverlayWindow()
{
    if (windows.empty())
        return;

    int anchorIdx = selected;
    if (anchorIdx < 0) anchorIdx = 0;
    if (anchorIdx >= static_cast<int>(windows.size())) anchorIdx = 0;
    HWND anchor = IsWindow(anchorWindow) ? anchorWindow : windows[anchorIdx];

    HMONITOR mon = GetAnchorMonitor(anchor);
    RECT work = GetWorkArea(mon);
    UINT dpi = GetDpiForHostOnMonitor(thumbHost, mon, work);
    scale = dpi / 96.0;

    int x, y, panelW, panelH;
    ComputeLayout(work, x, y, panelW, panelH);

    EnsureFont();

    icons.clear();
    for (HWND w : windows)
        icons.push_back(GetWindowIcon(w));

    ovX = x; ovY = y; ovW = panelW; ovH = panelH;
    bool magentaDebug = GetEnvironmentVariableW(L"AWC_MAGENTA", nullptr, 0) != 0;
    bool translucentBackdrop = GetEnvironmentVariableW(L"AWC_TRANSLUCENT_BACKDROP", nullptr, 0) != 0;
    bool noThumbnails = GetEnvironmentVariableW(L"AWC_NO_THUMBNAILS", nullptr, 0) != 0;
    bool whitePreview = GetEnvironmentVariableW(L"AWC_WHITE_PREVIEW", nullptr, 0) != 0;
    bool forceDwmThumbnails = GetEnvironmentVariableW(L"AWC_DWM_THUMBNAILS", nullptr, 0) != 0;
    snapshotThumbnails = !forceDwmThumbnails && !noThumbnails && !whitePreview;

    if (snapshotThumbnails)
        CaptureSnapshots();
    else
        ClearSnapshots();

    // Stacked, non-activating popups:
    //   backdropHost (optional, bottom): simple translucent rounded background.
    //   thumbHost    (middle): acrylic backdrop in normal mode, or legacy DWM thumbnails.
    //   overlay      (front): cards/header/ring and default same-surface masked previews.
    //
    // Important: in translucent-backdrop diagnostics, the backdrop must be separate
    // from the app thumbnails. Making the overlay panel translucent affects the same
    // composition path as the preview apertures and makes it look like the app is
    // transparent. A separate bottom host keeps the background translucent while the
    // DWM thumbnail layer remains fully opaque above it.
    if (translucentBackdrop)
    {
        SetWindowPos(backdropHost, HWND_TOPMOST, x, y, panelW, panelH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RenderBackdrop();
    }
    else
    {
        ShowWindow(backdropHost, SW_HIDE);
    }

    bool showThumbHost = !translucentBackdrop || !snapshotThumbnails;
    if (showThumbHost)
    {
        SetWindowPos(thumbHost, HWND_TOPMOST, x, y, panelW, panelH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!magentaDebug && !translucentBackdrop)
            EnableAcrylic(thumbHost, GetAcrylicGradientABGR());
        DWORD cornerPref = DWMWCP_ROUND;
        DwmSetWindowAttribute(thumbHost, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &cornerPref, sizeof(cornerPref));
        HRGN rgn = nullptr;
        if (translucentBackdrop)
        {
            // With the backdrop drawn by a separate bottom host, the thumbnail host should
            // occupy only thumbnail areas; otherwise its normal window background covers
            // the translucent panel/gaps.
            rgn = CreateRectRgn(0, 0, 0, 0);
            const int topOver = (std::max)(3, Scaled(3));
            const int rr = radius + Scaled(4);
            for (int i = 0; i < static_cast<int>(windows.size()); ++i)
            {
                RECT pv = PreviewRect(TileRect(i));
                HRGN topRgn = CreateRectRgn(pv.left, pv.top - topOver, pv.right, pv.bottom - rr);
                HRGN botRgn = CreateRoundRectRgn(pv.left, pv.bottom - 2 * rr,
                                                 pv.right + 1, pv.bottom + 1,
                                                 2 * rr, 2 * rr);
                HRGN tileRgn = CreateRectRgn(0, 0, 0, 0);
                CombineRgn(tileRgn, topRgn, botRgn, RGN_OR);
                CombineRgn(rgn, rgn, tileRgn, RGN_OR);
                DeleteObject(topRgn);
                DeleteObject(botRgn);
                DeleteObject(tileRgn);
            }
        }
        else
        {
            rgn = CreateRoundRectRgn(0, 0, panelW + 1, panelH + 1,
                                     2 * Scaled(12), 2 * Scaled(12));
        }
        SetWindowRgn(thumbHost, rgn, FALSE); // window owns rgn now
        RedrawWindow(thumbHost, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    else
    {
        ShowWindow(thumbHost, SW_HIDE);
    }
    RegisterThumbnails();

    SetWindowPos(overlay, HWND_TOPMOST, x, y, panelW, panelH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RenderLayered();
    if (showThumbHost)
    {
        // Keep the host directly behind the overlay in the topmost band.
        SetWindowPos(thumbHost, overlay, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }
    if (translucentBackdrop)
    {
        SetWindowPos(backdropHost, showThumbHost ? thumbHost : overlay, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }
}

void Switcher::HideOverlayWindow()
{
    ShowWindow(overlay, SW_HIDE);
    ShowWindow(thumbHost, SW_HIDE);
    ShowWindow(backdropHost, SW_HIDE);
    UnregisterThumbnails();
    ClearSnapshots();
    snapshotThumbnails = false;
    icons.clear();
}

void Switcher::SetSelection(int index)
{
    selected = index;
    RenderLayered();
}

void Switcher::ComputeLayout(const RECT& work, int& x, int& y, int& panelW, int& panelH)
{
    pad = Scaled(32);
    gap = Scaled(26);
    tileW = Scaled(300);
    headerH = Scaled(44);
    previewH = Scaled(176);
    // Win11 Alt-Tab thumbnails align with the card/header edges. The rounded bottom
    // corners are produced by the layered overlay mask, not by insetting the image.
    inner = 0;
    radius = Scaled(8);
    cardTrimBottom = radius;
    iconSize = Scaled(24);
    tileH = headerH + previewH;

    int workW = work.right - work.left;
    int workH = work.bottom - work.top;
    int n = static_cast<int>(windows.size());

    int maxCols = (std::max)(1, (workW - 2 * pad + gap) / (tileW + gap));
    cols = (std::min)(n, (std::min)(MAX_COLS, maxCols));
    if (cols < 1) cols = 1;
    rows = (n + cols - 1) / cols;

    panelW = 2 * pad + cols * tileW + (cols - 1) * gap;
    panelH = 2 * pad + rows * tileH + (rows - 1) * gap;

    x = work.left + (workW - panelW) / 2;
    y = work.top + (workH - panelH) / 2;
}

RECT Switcher::TileRect(int index) const
{
    int col = index % cols;
    int row = index / cols;
    int left = pad + col * (tileW + gap);
    int top = pad + row * (tileH + gap);
    RECT r = { left, top, left + tileW, top + tileH - cardTrimBottom };
    return r;
}

// The live thumbnail sits below the header "tab" edge-to-edge with the card.
// DWM thumbnails render as rectangles even when the host has a rounded region, so
// the overlay paints bottom-corner covers to make the live preview read as rounded.
RECT Switcher::PreviewRect(const RECT& tile) const
{
    RECT r = { tile.left + inner, tile.top + headerH, tile.right - inner, tile.bottom - inner };
    return r;
}

// The header "tab" across the top of the tile: app icon + window title.
RECT Switcher::HeaderRect(const RECT& tile) const
{
    int m = Scaled(12);
    RECT r = { tile.left + m, tile.top, tile.right - m, tile.top + headerH };
    return r;
}

// Cover-crop the destination's aspect ratio against an available SOURCE region
// (the window's visible bounds, excluding the invisible resize border) so the
// thumbnail FILLS the cell edge-to-edge (Win11 "cover") with no letterboxing or
// white frame strips.
static RECT CoverSource(const RECT& dest, const RECT& avail)
{
    int aw = avail.right - avail.left;
    int ah = avail.bottom - avail.top;
    int dw = dest.right - dest.left;
    int dh = dest.bottom - dest.top;
    if (aw <= 0 || ah <= 0 || dw <= 0 || dh <= 0)
        return avail;

    double destA = static_cast<double>(dw) / dh;
    double srcA = static_cast<double>(aw) / ah;
    if (srcA > destA)
    {
        int cw = (std::max)(1, static_cast<int>(ah * destA + 0.5));
        int x = avail.left + (aw - cw) / 2;
        return { x, avail.top, x + cw, avail.bottom };
    }
    else
    {
        int ch = (std::max)(1, static_cast<int>(aw / destA + 0.5));
        int y = avail.top + (ah - ch) / 2;
        return { avail.left, y, avail.right, y + ch };
    }
}

static SIZE QueryThumbSize(HTHUMBNAIL th)
{
    SIZE s = { 0, 0 };
    DwmQueryThumbnailSourceSize(th, &s);
    return s;
}

// The source window's CLIENT-area size (physical pixels). With
// fSourceClientAreaOnly=TRUE this is the coordinate space DWM samples from, so
// cover-cropping against it excludes the non-client frame (title bar + the ~7px
// invisible resize border that otherwise composites as a white edge strip).
static SIZE ClientSourceSize(HWND hwnd)
{
    RECT cr = {};
    if (GetClientRect(hwnd, &cr))
    {
        SIZE s = { cr.right - cr.left, cr.bottom - cr.top };
        if (s.cx > 0 && s.cy > 0)
            return s;
    }
    return { 0, 0 };
}

static SnapshotThumb CaptureWindowClientSnapshot(HWND hwnd)
{
    SnapshotThumb snap;

    RECT cr = {};
    if (!GetClientRect(hwnd, &cr))
        return snap;

    int w = cr.right - cr.left;
    int h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0)
        return snap;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    if (!mem)
    {
        ReleaseDC(nullptr, screen);
        return snap;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
    {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return snap;
    }

    HGDIOBJ oldBmp = SelectObject(mem, dib);
    ZeroMemory(bits, static_cast<size_t>(w) * h * 4);

    BOOL ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    if (!ok && !IsIconic(hwnd))
    {
        POINT pt = { 0, 0 };
        if (ClientToScreen(hwnd, &pt))
        {
            HDC windowDC = GetDC(nullptr);
            ok = BitBlt(mem, 0, 0, w, h, windowDC, pt.x, pt.y, SRCCOPY);
            ReleaseDC(nullptr, windowDC);
        }
    }

    if (ok)
    {
        BYTE* px = reinterpret_cast<BYTE*>(bits);
        for (size_t i = 0, count = static_cast<size_t>(w) * h; i < count; ++i)
            px[i * 4 + 3] = 255;

        snap.bitmap = dib;
        snap.bits = bits;
        snap.width = w;
        snap.height = h;
    }

    SelectObject(mem, oldBmp);
    if (!ok)
        DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return snap;
}

static void ClearPreviewBottomCorners(Gdiplus::Graphics& g, const RECT& pv, int radius)
{
    Gdiplus::REAL r = static_cast<Gdiplus::REAL>(radius);
    if (r <= 0)
        return;

    Gdiplus::REAL L = static_cast<Gdiplus::REAL>(pv.left);
    Gdiplus::REAL R = static_cast<Gdiplus::REAL>(pv.right);
    Gdiplus::REAL B = static_cast<Gdiplus::REAL>(pv.bottom);
    Gdiplus::SolidBrush clearBrush(AltTabStyle::Transparent());

    Gdiplus::GraphicsState state = g.Save();
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);

    Gdiplus::GraphicsPath bottomL;
    bottomL.AddLine(L, B - r, L, B);
    bottomL.AddLine(L, B, L + r, B);
    bottomL.AddArc(L, B - 2 * r, 2 * r, 2 * r, 90, 90);
    bottomL.CloseFigure();
    g.FillPath(&clearBrush, &bottomL);

    Gdiplus::GraphicsPath bottomR;
    bottomR.AddLine(R, B - r, R, B);
    bottomR.AddLine(R, B, R - r, B);
    bottomR.AddArc(R - 2 * r, B - 2 * r, 2 * r, 2 * r, 90, -90);
    bottomR.CloseFigure();
    g.FillPath(&clearBrush, &bottomR);

    g.Restore(state);
}

void Switcher::ClearSnapshots()
{
    for (SnapshotThumb& snap : snapshots)
    {
        if (snap.bitmap)
            DeleteObject(snap.bitmap);
    }
    snapshots.clear();
}

void Switcher::CaptureSnapshots()
{
    ClearSnapshots();
    snapshots.reserve(windows.size());
    for (HWND hwnd : windows)
        snapshots.push_back(CaptureWindowClientSnapshot(hwnd));
}

// Register a live DWM thumbnail for each window against the shared host window.
// Each is positioned (in host/overlay-client coords) over its tile's preview rect
// and cover-cropped to fill it. DWM composites these on the GPU, so it's instant.
// fSourceClientAreaOnly excludes the window frame so there is no white border edge.
void Switcher::RegisterThumbnails()
{
    UnregisterThumbnails();
    // Debug modes: either remove the preview entirely, or draw a synthetic white
    // preview rectangle so mask geometry can be diagnosed without DWM thumbnails.
    if (snapshotThumbnails ||
        GetEnvironmentVariableW(L"AWC_NO_THUMBNAILS", nullptr, 0) ||
        GetEnvironmentVariableW(L"AWC_WHITE_PREVIEW", nullptr, 0))
        return;

    // Keep the thumbnail edge-to-edge with the card. Only the top is overscanned so
    // its soft DWM edge tucks under the opaque header tab; side/bottom overscan would
    // leak into the gaps because public DWM thumbnails ignore window regions.
    const int topOver = (std::max)(3, Scaled(3));
    for (size_t i = 0; i < windows.size(); ++i)
    {
        RECT dest = PreviewRect(TileRect(static_cast<int>(i)));
        dest.top -= topOver;
        HTHUMBNAIL th = nullptr;
        if (FAILED(DwmRegisterThumbnail(thumbHost, windows[i], &th)) || !th)
        {
            thumbs.push_back(nullptr);
            continue;
        }
        thumbs.push_back(th);

        // Cover-crop against the client area (fSourceClientAreaOnly excludes the
        // window frame, so there is no white invisible-border edge). Minimized
        // windows report a bogus tiny placeholder client rect, so fall back to the
        // full retained thumbnail source for them.
        SIZE csz = IsIconic(windows[i]) ? SIZE{ 0, 0 } : ClientSourceSize(windows[i]);
        BOOL clientOnly = TRUE;
        if (csz.cx <= 0 || csz.cy <= 0)
        {
            csz = QueryThumbSize(th);
            clientOnly = FALSE;
        }
        RECT avail = { 0, 0, csz.cx, csz.cy };
        // Inset the available source a couple pixels before cover-cropping. DWM
        // downscales the source (~1800px) to the small dest (~450px) with bilinear
        // sampling; if the client's extreme edge column is a 1px light frame/focus
        // line (common on Chromium/Electron windows like VS Code) it bleeds into
        // the dest's edge column as a light "sliver". Dropping the outermost edge
        // pixels removes that without any visible content loss.
        if (clientOnly)
        {
            int ix = (std::min)(2, (int)(avail.right - avail.left) / 4);
            int iy = (std::min)(2, (int)(avail.bottom - avail.top) / 4);
            avail.left += ix; avail.right -= ix;
            avail.top += iy; avail.bottom -= iy;
        }
        RECT rcSrc = CoverSource(dest, avail);
        DWM_THUMBNAIL_PROPERTIES props = {};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE |
                        DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        props.rcDestination = dest;
        props.rcSource = rcSrc;
        props.opacity = 255;
        props.fVisible = TRUE;
        props.fSourceClientAreaOnly = clientOnly;
        DwmUpdateThumbnailProperties(th, &props);
    }
}

void Switcher::UnregisterThumbnails()
{
    for (HTHUMBNAIL th : thumbs)
        if (th)
            DwmUnregisterThumbnail(th);
    thumbs.clear();
}

void Switcher::EnsureFont()
{
    if (font && fontScale == scale)
        return;
    if (font)
    {
        DeleteObject(font);
        font = nullptr;
    }
    int height = -Scaled(15);
    BYTE quality = scale > 1.01 ? ANTIALIASED_QUALITY : CLEARTYPE_NATURAL_QUALITY;
    font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       quality, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    fontScale = scale;
}

static void DrawHeaderText(HDC dc, HFONT font, const RECT& rc, const std::wstring& text, COLORREF color)
{
    if (text.empty() || !font)
        return;

    HGDIOBJ oldFont = SelectObject(dc, font);
    int oldBk = SetBkMode(dc, TRANSPARENT);
    COLORREF oldColor = SetTextColor(dc, color);

    RECT textRc = rc;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRc,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldBk);
    SelectObject(dc, oldFont);
}

static std::wstring GetTitle(HWND hwnd)
{
    wchar_t buf[256];
    int len = GetWindowTextW(hwnd, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, len > 0 ? len : 0);
}

// Borrowed icon handle for a window (do NOT DestroyIcon these).
static HICON GetWindowIcon(HWND hwnd)
{
    DWORD_PTR res = 0;
    HICON icon = nullptr;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 100, &res) && res)
        icon = reinterpret_cast<HICON>(res);
    if (!icon && SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &res) && res)
        icon = reinterpret_cast<HICON>(res);
    if (!icon)
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    if (!icon)
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    return icon;
}

// The Windows accent color used to outline the selected tile (matches the real
// Win11 Alt-Tab selection). Reads HKCU\...\DWM\AccentColor (REG_DWORD, stored as
// 0xAABBGGRR, which masks directly to a COLORREF), with DWM colorization and a
// default Win11 blue as fallbacks.
static COLORREF GetAccentColor()
{
    DWORD color = 0, size = sizeof(color), type = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        LONG r = RegQueryValueExW(key, L"AccentColor", nullptr, &type,
                                  reinterpret_cast<BYTE*>(&color), &size);
        RegCloseKey(key);
        if (r == ERROR_SUCCESS && type == REG_DWORD)
            return color & 0x00FFFFFF;
    }

    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)))
        return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);

    return AltTabStyle::AccentFallbackRef();
}

// Build a rounded-rectangle GDI+ path.
static void BuildRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, Gdiplus::REAL rad)
{
    path.Reset();
    Gdiplus::REAL d = rad * 2;
    if (d <= 0 || d > r.Width || d > r.Height)
    {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

static void BuildTopRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, Gdiplus::REAL rad)
{
    path.Reset();
    Gdiplus::REAL d = rad * 2;
    if (d <= 0 || d > r.Width || d > r.Height)
    {
        path.AddRectangle(r);
        return;
    }

    path.StartFigure();
    path.AddLine(r.X, r.GetBottom(), r.X, r.Y + rad);
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddLine(r.X + rad, r.Y, r.GetRight() - rad, r.Y);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddLine(r.GetRight(), r.Y + rad, r.GetRight(), r.GetBottom());
    path.AddLine(r.GetRight(), r.GetBottom(), r.X, r.GetBottom());
    path.CloseFigure();
}

static Gdiplus::RectF InflateF(const RECT& r, int by)
{
    return Gdiplus::RectF(
        static_cast<Gdiplus::REAL>(r.left - by),
        static_cast<Gdiplus::REAL>(r.top - by),
        static_cast<Gdiplus::REAL>((r.right - r.left) + 2 * by),
        static_cast<Gdiplus::REAL>((r.bottom - r.top) + 2 * by));
}

// Draw an HICON into the layered-window DIB without giving transparent icon pixels
// an opaque matte. DrawIconEx can disturb the alpha channel on a 32-bpp DIB, so draw
// into a scratch DIB and alpha-composite only the icon pixels over the already-opaque
// card/header background.
static void DrawIconOverPARGB(void* destBits, int destW, int destH,
                              HICON icon, int x, int y, int size)
{
    if (!destBits || !icon || destW <= 0 || destH <= 0 || size <= 0)
        return;

    HDC screen = GetDC(nullptr);
    HDC iconDC = CreateCompatibleDC(screen);
    if (!iconDC)
    {
        ReleaseDC(nullptr, screen);
        return;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* iconBits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &iconBits, nullptr, 0);
    if (!dib)
    {
        DeleteDC(iconDC);
        ReleaseDC(nullptr, screen);
        return;
    }

    HGDIOBJ oldBmp = SelectObject(iconDC, dib);
    ZeroMemory(iconBits, static_cast<size_t>(size) * size * 4);
    DrawIconEx(iconDC, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);

    BYTE* src = reinterpret_cast<BYTE*>(iconBits);
    BYTE* dst = reinterpret_cast<BYTE*>(destBits);

    bool hasAlpha = false;
    for (int i = 0; i < size * size; ++i)
    {
        if (src[i * 4 + 3] != 0)
        {
            hasAlpha = true;
            break;
        }
    }

    for (int sy = 0; sy < size; ++sy)
    {
        int dy = y + sy;
        if (dy < 0 || dy >= destH)
            continue;
        for (int sx = 0; sx < size; ++sx)
        {
            int dx = x + sx;
            if (dx < 0 || dx >= destW)
                continue;

            BYTE* s = src + (static_cast<size_t>(sy) * size + sx) * 4;
            BYTE* d = dst + (static_cast<size_t>(dy) * destW + dx) * 4;

            if (hasAlpha)
            {
                int a = s[3];
                if (a == 0)
                    continue;

                int sb = s[0];
                int sg = s[1];
                int sr = s[2];
                if (sb > a || sg > a || sr > a)
                {
                    sb = (sb * a + 127) / 255;
                    sg = (sg * a + 127) / 255;
                    sr = (sr * a + 127) / 255;
                }

                int inv = 255 - a;
                d[0] = static_cast<BYTE>((std::min)(255, sb + (d[0] * inv + 127) / 255));
                d[1] = static_cast<BYTE>((std::min)(255, sg + (d[1] * inv + 127) / 255));
                d[2] = static_cast<BYTE>((std::min)(255, sr + (d[2] * inv + 127) / 255));
                d[3] = 255;
            }
            else if (s[0] || s[1] || s[2])
            {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
            }
        }
    }

    SelectObject(iconDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(iconDC);
    ReleaseDC(nullptr, screen);
}

void Switcher::RenderBackdrop()
{
    int w = ovW;
    int h = ovH;
    if (!backdropHost || w <= 0 || h <= 0)
        return;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
    {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    ZeroMemory(bits, static_cast<size_t>(w) * h * 4);

    {
        Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB,
                            reinterpret_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        Gdiplus::GraphicsPath panel;
        RECT panelRect = { 0, 0, w, h };
        BuildRoundRect(panel, InflateF(panelRect, 0),
                       static_cast<Gdiplus::REAL>(Scaled(12)));
        Gdiplus::SolidBrush brush(AltTabStyle::TranslucentBackdrop());
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.FillPath(&brush, &panel);
        g.Flush();
    }

    POINT ptDst = { ovX, ovY };
    SIZE sz = { w, h };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(backdropHost, screenDC, &ptDst, &sz, memDC, &ptSrc, 0,
                        &bf, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// Renders the overlay chrome and default same-surface preview snapshots into a
// 32-bpp premultiplied top-down DIB, then pushes it via UpdateLayeredWindow. Keeping
// thumbnails in this DIB lets the bottom corners be a real transparent alpha mask
// over the acrylic backdrop. The legacy DWM-thumbnail path is still available via
// AWC_DWM_THUMBNAILS for comparison, but it needs opaque corner covers.
void Switcher::RenderLayered()
{
    int w = ovW;
    int h = ovH;
    if (w <= 0 || h <= 0)
        return;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib)
    {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    ZeroMemory(bits, static_cast<size_t>(w) * h * 4);

    {
        Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB,
                            reinterpret_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // Grayscale AA (not ClearType) avoids colored fringing over transparency.
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        bool magentaDebug = GetEnvironmentVariableW(L"AWC_MAGENTA", nullptr, 0) != 0;
        bool translucentBackdrop = GetEnvironmentVariableW(L"AWC_TRANSLUCENT_BACKDROP", nullptr, 0) != 0;
        bool noThumbnails = GetEnvironmentVariableW(L"AWC_NO_THUMBNAILS", nullptr, 0) != 0;
        bool whitePreview = GetEnvironmentVariableW(L"AWC_WHITE_PREVIEW", nullptr, 0) != 0;

        // Leave the overlay panel transparent. Acrylic lives on thumbHost in normal
        // mode; the simple translucent diagnostic backdrop lives on backdropHost.
        Gdiplus::REAL panelRadius = static_cast<Gdiplus::REAL>(Scaled(12));
        RECT panelRect = { 0, 0, w, h };
        Gdiplus::GraphicsPath panel;
        BuildRoundRect(panel, InflateF(panelRect, 0), panelRadius);
        Gdiplus::SolidBrush panelBrush(AltTabStyle::Transparent());
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.FillPath(&panelBrush, &panel);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

        COLORREF accent = GetAccentColor();
        Gdiplus::Color accentClr = AltTabStyle::Accent(accent);

        for (int i = 0; i < static_cast<int>(windows.size()); ++i)
        {
            RECT tile = TileRect(i);
            bool sel = (i == selected);
            RECT pv = PreviewRect(tile);

            // Opaque top tab only. Do not draw a full-height card behind the preview:
            // that was the stray opaque element still visible at the thumbnail bottom.
            RECT tab = { tile.left, tile.top, tile.right, pv.top };
            Gdiplus::GraphicsPath tabPath;
            BuildTopRoundRect(tabPath, InflateF(tab, 0), static_cast<Gdiplus::REAL>(radius));
            Gdiplus::SolidBrush tabBrush(AltTabStyle::Card(sel));
            g.FillPath(&tabBrush, &tabPath);

            // Same-surface previews can be alpha-masked for real. Public DWM live
            // thumbnails are a separate HWND composition layer, so that path still
            // needs opaque corner covers.
            int pw = pv.right - pv.left;
            int ph = pv.bottom - pv.top;
            if (!noThumbnails && pw > 0 && ph > 0)
            {
                Gdiplus::REAL r = static_cast<Gdiplus::REAL>(radius + Scaled(4));
                if (whitePreview)
                {
                    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
                    Gdiplus::SolidBrush previewBrush(AltTabStyle::WhitePreview());
                    g.FillRectangle(&previewBrush,
                                    static_cast<Gdiplus::REAL>(pv.left),
                                    static_cast<Gdiplus::REAL>(pv.top),
                                    static_cast<Gdiplus::REAL>(pw),
                                    static_cast<Gdiplus::REAL>(ph));
                    ClearPreviewBottomCorners(g, pv, static_cast<int>(r));
                    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
                }
                else if (snapshotThumbnails)
                {
                    if (i < static_cast<int>(snapshots.size()) &&
                        snapshots[i].bitmap && snapshots[i].bits &&
                        snapshots[i].width > 0 && snapshots[i].height > 0)
                    {
                        SnapshotThumb& snap = snapshots[i];
                        RECT avail = { 0, 0, snap.width, snap.height };
                        int ix = (std::min)(2, static_cast<int>(avail.right - avail.left) / 4);
                        int iy = (std::min)(2, static_cast<int>(avail.bottom - avail.top) / 4);
                        avail.left += ix; avail.right -= ix;
                        avail.top += iy; avail.bottom -= iy;
                        RECT src = CoverSource(pv, avail);

                        Gdiplus::Bitmap srcBmp(snap.width, snap.height, snap.width * 4,
                                               PixelFormat32bppRGB,
                                               reinterpret_cast<BYTE*>(snap.bits));
                        Gdiplus::GraphicsState saved = g.Save();
                        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
                        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                        Gdiplus::Rect dest(pv.left, pv.top, pw, ph);
                        g.DrawImage(&srcBmp, dest,
                                    src.left, src.top, src.right - src.left, src.bottom - src.top,
                                    Gdiplus::UnitPixel);
                        g.Restore(saved);
                        ClearPreviewBottomCorners(g, pv, static_cast<int>(r));
                    }
                }
                else
                {
                    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
                    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
                    Gdiplus::SolidBrush previewBrush(AltTabStyle::Transparent());
                    g.FillRectangle(&previewBrush,
                                    static_cast<Gdiplus::REAL>(pv.left),
                                    static_cast<Gdiplus::REAL>(pv.top),
                                    static_cast<Gdiplus::REAL>(pw),
                                    static_cast<Gdiplus::REAL>(ph));
                    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

                    Gdiplus::REAL cover = static_cast<Gdiplus::REAL>((std::max)(1, Scaled(1)));
                    Gdiplus::REAL L = static_cast<Gdiplus::REAL>(pv.left);
                    Gdiplus::REAL R = static_cast<Gdiplus::REAL>(pv.right);
                    Gdiplus::REAL B = static_cast<Gdiplus::REAL>(pv.bottom);
                    Gdiplus::REAL Lc = L - cover;
                    Gdiplus::REAL Rc = R + cover;
                    Gdiplus::REAL Bc = B + cover;
                    Gdiplus::SolidBrush maskBrush(
                        AltTabStyle::PreviewMask(sel, magentaDebug, translucentBackdrop));
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

                    Gdiplus::GraphicsPath bottomL;
                    bottomL.AddLine(Lc, Bc - r, Lc, Bc);
                    bottomL.AddLine(Lc, Bc, Lc + r, Bc);
                    bottomL.AddArc(Lc, Bc - 2 * r, 2 * r, 2 * r, 90, 90);
                    bottomL.CloseFigure();
                    g.FillPath(&maskBrush, &bottomL);

                    Gdiplus::GraphicsPath bottomR;
                    bottomR.AddLine(Rc, Bc - r, Rc, Bc);
                    bottomR.AddLine(Rc, Bc, Rc - r, Bc);
                    bottomR.AddArc(Rc - 2 * r, Bc - 2 * r, 2 * r, 2 * r, 90, -90);
                    bottomR.CloseFigure();
                    g.FillPath(&maskBrush, &bottomR);
                }

            }

            // Header "tab": app icon + window title.
            RECT hdr = HeaderRect(tile);
            int textLeft = hdr.left;
            HICON ic = (i < static_cast<int>(icons.size())) ? icons[i] : nullptr;
            if (ic)
            {
                int iy = tile.top + (headerH - iconSize) / 2;
                g.Flush();
                DrawIconOverPARGB(bits, w, h, ic, hdr.left, iy, iconSize);
                textLeft = hdr.left + iconSize + Scaled(8);
            }

            std::wstring text = GetTitle(windows[i]);
            g.Flush();
            RECT textRc = { textLeft, tile.top, hdr.right, tile.top + headerH };
            DrawHeaderText(memDC, font, textRc, text, AltTabStyle::HeaderTextRef(sel));

            // Two-ring focus with a clear padding gap to the tile.
            if (sel)
            {
                int gPad = Scaled(10);
                int gOut = gPad + Scaled(2);
                Gdiplus::GraphicsPath inr, out;
                BuildRoundRect(inr, InflateF(tile, gPad),
                               static_cast<Gdiplus::REAL>(radius + gPad));
                BuildRoundRect(out, InflateF(tile, gOut),
                               static_cast<Gdiplus::REAL>(radius + gOut));
                Gdiplus::Pen darkPen(AltTabStyle::FocusShadow(),
                                     static_cast<Gdiplus::REAL>((std::max)(1, Scaled(1))));
                Gdiplus::Pen accentPen(accentClr,
                                       static_cast<Gdiplus::REAL>((std::max)(2, Scaled(3))));
                g.DrawPath(&darkPen, &inr);
                g.DrawPath(&accentPen, &out);
            }
        }

        g.Flush();
    }

    POINT ptDst = { ovX, ovY };
    SIZE sz = { w, h };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(overlay, screenDC, &ptDst, &sz, memDC, &ptSrc, 0,
                        &bf, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

LRESULT CALLBACK Switcher::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Switcher* self = reinterpret_cast<Switcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self)
    {
        switch (msg)
        {
        case WM_TIMER:
            if (wParam == TIMER_ID)
            {
                self->OnTick();
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1; // layered window; content pushed via UpdateLayeredWindow
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// =============================== Self-test ===================================

struct AllCtx
{
    std::vector<std::pair<std::wstring, HWND>> items;
};

static BOOL CALLBACK EnumAll(HWND hwnd, LPARAM lp)
{
    if (!IsAltTabWindow(hwnd))
        return TRUE;
    AllCtx* c = reinterpret_cast<AllCtx*>(lp);
    c->items.push_back({ ProcessImagePath(hwnd), hwnd });
    return TRUE;
}

void Switcher::RunSelfTest()
{
    AllCtx all;
    EnumWindows(EnumAll, reinterpret_cast<LPARAM>(&all));

    // Pick the process image path that owns the most Alt-Tab windows (>= 2).
    std::wstring best;
    size_t bestCount = 0;
    for (const auto& a : all.items)
    {
        size_t count = 0;
        for (const auto& b : all.items)
            if (_wcsicmp(a.first.c_str(), b.first.c_str()) == 0)
                ++count;
        if (count > bestCount)
        {
            bestCount = count;
            best = a.first;
        }
    }

    if (bestCount < 2)
    {
        MessageBoxW(nullptr, L"Self-test needs an app with >= 2 windows open.",
                    L"AltWindowCycle", MB_OK | MB_ICONINFORMATION);
        return;
    }

    windows.clear();
    for (const auto& a : all.items)
        if (_wcsicmp(a.first.c_str(), best.c_str()) == 0)
            windows.push_back(a.second);
    std::sort(windows.begin(), windows.end());
    selected = windows.size() > 1 ? 1 : 0;

    ShowOverlayWindow();

    DWORD end = GetTickCount() + 4000;
    MSG m;
    while (GetTickCount() < end)
    {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(15);
    }

    HideOverlayWindow();
}

// =============================== Entry point =================================

static Switcher g_switcher;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int)
{
    // Single instance: bail if another copy already owns the hotkeys.
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"AltWindowCycle_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    // Per-monitor DPI so overlay layout and DWM thumbnail rects line up on any monitor.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // GDI+ + buffered paint power the alpha-correct chrome over the acrylic backdrop.
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);
    BufferedPaintInit();

    // Overlay is on by default; --no-overlay restores the instant-switch behavior.
    if (lpCmdLine && wcsstr(lpCmdLine, L"--no-overlay"))
        g_switcher.showOverlay = false;

    if (!g_switcher.Init(hInstance))
    {
        MessageBoxW(nullptr, L"Failed to create overlay window.", L"AltWindowCycle", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (lpCmdLine && wcsstr(lpCmdLine, L"--selftest"))
    {
        g_switcher.RunSelfTest();
        g_switcher.Shutdown();
        BufferedPaintUnInit();
        Gdiplus::GdiplusShutdown(gdipToken);
        if (mtx)
            CloseHandle(mtx);
        return 0;
    }

    const UINT vkBacktick = VK_OEM_3; // ` ~ on US layouts
    if (!RegisterHotKey(nullptr, HK_FORWARD, MOD_ALT | MOD_NOREPEAT, vkBacktick) ||
        !RegisterHotKey(nullptr, HK_BACKWARD, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, vkBacktick))
    {
        MessageBoxW(nullptr, L"Failed to register Alt+` hotkey (already in use?).",
                    L"AltWindowCycle", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_HOTKEY)
        {
            if (msg.wParam == HK_FORWARD)
                g_switcher.OnHotkey(true);
            else if (msg.wParam == HK_BACKWARD)
                g_switcher.OnHotkey(false);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(nullptr, HK_FORWARD);
    UnregisterHotKey(nullptr, HK_BACKWARD);
    g_switcher.Shutdown();
    BufferedPaintUnInit();
    Gdiplus::GdiplusShutdown(gdipToken);
    if (mtx)
        CloseHandle(mtx);
    return 0;
}
