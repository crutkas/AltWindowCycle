// AltWindowCycle (C++ POC)
// macOS Cmd+` behavior on Windows: cycle top-level windows of the *foreground app*.
//   Alt+`        -> next window of same app
//   Shift+Alt+`  -> previous window
//
// Tiny, dependency-free Win32. No visible window; runs in the message loop.
// Build: see build.ps1 (cl /O1 /Os /MT, /SUBSYSTEM:WINDOWS).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <vector>
#include <algorithm>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

static const int HK_FORWARD = 1;
static const int HK_BACKWARD = 2;

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

static void CycleWindows(bool forward)
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return;

    EnumCtx ctx;
    ctx.exe = ProcessImagePath(fg);
    if (ctx.exe.empty())
        return;

    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));

    const size_t n = ctx.windows.size();
    if (n < 2)
        return;

    // Stable order independent of activation, so repeated presses cycle all.
    std::sort(ctx.windows.begin(), ctx.windows.end());

    size_t idx = 0;
    bool found = false;
    for (size_t i = 0; i < n; ++i)
    {
        if (ctx.windows[i] == fg)
        {
            idx = i;
            found = true;
            break;
        }
    }

    size_t target;
    if (!found)
        target = 0;
    else if (forward)
        target = (idx + 1) % n;
    else
        target = (idx + n - 1) % n;

    ForceForeground(ctx.windows[target]);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // Single instance: bail if another copy already owns the hotkeys.
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"AltWindowCycle_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

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
                CycleWindows(true);
            else if (msg.wParam == HK_BACKWARD)
                CycleWindows(false);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(nullptr, HK_FORWARD);
    UnregisterHotKey(nullptr, HK_BACKWARD);
    if (mtx)
        CloseHandle(mtx);
    return 0;
}
