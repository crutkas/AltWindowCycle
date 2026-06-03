using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace AltWindowCycle;

// macOS Cmd+` behavior on Windows: cycle top-level windows of the foreground app.
//   Alt+`        -> next window of same app
//   Shift+Alt+`  -> previous window
//
// Hidden message-only window; no UI. Mirrors the C++ POC's algorithm.
internal static class Program
{
    private const int HK_FORWARD = 1;
    private const int HK_BACKWARD = 2;

    [STAThread]
    private static int Main()
    {
        using var _ = new Mutex(true, "AltWindowCycle_SingleInstance", out bool createdNew);
        if (!createdNew)
            return 0;

        using var window = new HotkeyWindow();
        if (!window.RegisterHotkeys())
        {
            Native.MessageBox(IntPtr.Zero, "Failed to register Alt+` hotkey (already in use?).",
                "AltWindowCycle", 0x10 /* MB_ICONERROR */);
            return 1;
        }

        // Standard Win32 message pump.
        while (Native.GetMessage(out Native.MSG msg, IntPtr.Zero, 0, 0) > 0)
        {
            Native.TranslateMessage(ref msg);
            Native.DispatchMessage(ref msg);
        }
        return 0;
    }

    // Keep the mutex alive for process lifetime.
    private sealed class Mutex : IDisposable
    {
        private readonly IntPtr _handle;
        public Mutex(bool initialOwner, string name, out bool createdNew)
        {
            _handle = Native.CreateMutex(IntPtr.Zero, initialOwner, name);
            createdNew = Marshal.GetLastWin32Error() != 183 /* ERROR_ALREADY_EXISTS */;
        }
        public void Dispose()
        {
            if (_handle != IntPtr.Zero) Native.CloseHandle(_handle);
        }
    }
}

internal sealed class HotkeyWindow : IDisposable
{
    private const int HK_FORWARD = 1;
    private const int HK_BACKWARD = 2;
    private const uint WM_HOTKEY = 0x0312;
    private const uint MOD_ALT = 0x0001;
    private const uint MOD_SHIFT = 0x0004;
    private const uint MOD_NOREPEAT = 0x4000;
    private const uint VK_OEM_3 = 0xC0; // ` ~ on US layouts

    private readonly IntPtr _hwnd;
    private readonly Native.WndProc _wndProc; // pinned via field to avoid GC
    private const string ClassName = "AltWindowCycleMsgWnd";

    public HotkeyWindow()
    {
        _wndProc = WindowProc;
        var wc = new Native.WNDCLASS
        {
            lpfnWndProc = _wndProc,
            hInstance = Native.GetModuleHandle(null),
            lpszClassName = ClassName,
        };
        Native.RegisterClass(ref wc);
        // HWND_MESSAGE (-3) = message-only window.
        _hwnd = Native.CreateWindowEx(0, ClassName, string.Empty, 0, 0, 0, 0, 0,
            new IntPtr(-3), IntPtr.Zero, wc.hInstance, IntPtr.Zero);
    }

    public bool RegisterHotkeys()
    {
        bool a = Native.RegisterHotKey(_hwnd, HK_FORWARD, MOD_ALT | MOD_NOREPEAT, VK_OEM_3);
        bool b = Native.RegisterHotKey(_hwnd, HK_BACKWARD, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_OEM_3);
        return a && b;
    }

    private IntPtr WindowProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        if (msg == WM_HOTKEY)
        {
            int id = wParam.ToInt32();
            if (id == HK_FORWARD) WindowCycler.Cycle(forward: true);
            else if (id == HK_BACKWARD) WindowCycler.Cycle(forward: false);
            return IntPtr.Zero;
        }
        return Native.DefWindowProc(hwnd, msg, wParam, lParam);
    }

    public void Dispose()
    {
        if (_hwnd != IntPtr.Zero)
        {
            Native.UnregisterHotKey(_hwnd, HK_FORWARD);
            Native.UnregisterHotKey(_hwnd, HK_BACKWARD);
            Native.DestroyWindow(_hwnd);
        }
    }
}

internal static class WindowCycler
{
    public static void Cycle(bool forward)
    {
        IntPtr fg = Native.GetForegroundWindow();
        if (fg == IntPtr.Zero) return;

        string fgExe = ProcessImagePath(fg);
        if (string.IsNullOrEmpty(fgExe)) return;

        var windows = new List<IntPtr>();
        Native.EnumWindows((hwnd, _) =>
        {
            if (!IsAltTabWindow(hwnd)) return true;
            string exe = ProcessImagePath(hwnd);
            if (!string.IsNullOrEmpty(exe) &&
                string.Equals(exe, fgExe, StringComparison.OrdinalIgnoreCase))
            {
                windows.Add(hwnd);
            }
            return true;
        }, IntPtr.Zero);

        int n = windows.Count;
        if (n < 2) return;

        // Stable order independent of activation, so repeated presses cycle all.
        windows.Sort((x, y) => x.ToInt64().CompareTo(y.ToInt64()));

        int idx = windows.IndexOf(fg);
        int target;
        if (idx < 0) target = 0;
        else if (forward) target = (idx + 1) % n;
        else target = (idx + n - 1) % n;

        ForceForeground(windows[target]);
    }

    private static string ProcessImagePath(IntPtr hwnd)
    {
        Native.GetWindowThreadProcessId(hwnd, out uint pid);
        if (pid == 0) return string.Empty;

        IntPtr proc = Native.OpenProcess(Native.PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (proc == IntPtr.Zero) return string.Empty;
        try
        {
            var sb = new StringBuilder(1024);
            int cap = sb.Capacity;
            return Native.QueryFullProcessImageName(proc, 0, sb, ref cap) ? sb.ToString() : string.Empty;
        }
        finally
        {
            Native.CloseHandle(proc);
        }
    }

    private static bool IsAltTabWindow(IntPtr hwnd)
    {
        if (!Native.IsWindowVisible(hwnd)) return false;

        if (Native.DwmGetWindowAttribute(hwnd, Native.DWMWA_CLOAKED, out int cloaked, sizeof(int)) == 0
            && cloaked != 0)
        {
            return false;
        }

        IntPtr walk = Native.GetAncestor(hwnd, Native.GA_ROOTOWNER);
        while (true)
        {
            IntPtr tryPopup = Native.GetLastActivePopup(walk);
            if (tryPopup == walk) break;
            if (Native.IsWindowVisible(tryPopup)) break;
            walk = tryPopup;
        }
        if (walk != hwnd) return false;

        long exStyle = Native.GetWindowLongPtr(hwnd, Native.GWL_EXSTYLE).ToInt64();
        if ((exStyle & Native.WS_EX_TOOLWINDOW) != 0) return false;

        return true;
    }

    private static void ForceForeground(IntPtr hwnd)
    {
        if (Native.IsIconic(hwnd))
            Native.ShowWindow(hwnd, Native.SW_RESTORE);

        uint fgThread = Native.GetWindowThreadProcessId(Native.GetForegroundWindow(), out _);
        uint myThread = Native.GetCurrentThreadId();

        bool attached = fgThread != 0 && fgThread != myThread;
        if (attached) Native.AttachThreadInput(myThread, fgThread, true);

        Native.BringWindowToTop(hwnd);
        Native.SetForegroundWindow(hwnd);
        Native.SetFocus(hwnd);

        if (attached) Native.AttachThreadInput(myThread, fgThread, false);
    }
}

internal static class Native
{
    public const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    public const int GWL_EXSTYLE = -20;
    public const long WS_EX_TOOLWINDOW = 0x00000080;
    public const uint GA_ROOTOWNER = 3;
    public const int DWMWA_CLOAKED = 14;
    public const int SW_RESTORE = 9;

    public delegate IntPtr WndProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct MSG
    {
        public IntPtr hwnd; public uint message; public IntPtr wParam;
        public IntPtr lParam; public uint time; public POINT pt;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct WNDCLASS
    {
        public uint style;
        public WndProc lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        public string lpszMenuName;
        public string lpszClassName;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr CreateMutex(IntPtr attr, bool initialOwner, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr GetModuleHandle(string? name);
    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool QueryFullProcessImageName(IntPtr proc, uint flags, StringBuilder buf, ref int size);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern ushort RegisterClass(ref WNDCLASS wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowEx(int exStyle, string cls, string name, uint style,
        int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")]
    public static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr DefWindowProc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);
    [DllImport("user32.dll")]
    public static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr DispatchMessage(ref MSG msg);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool RegisterHotKey(IntPtr hwnd, int id, uint mods, uint vk);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool UnregisterHotKey(IntPtr hwnd, int id);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern IntPtr SetFocus(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
    [DllImport("user32.dll")]
    public static extern IntPtr GetLastActivePopup(IntPtr hwnd);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtr(IntPtr hwnd, int index);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int MessageBox(IntPtr hwnd, string text, string caption, uint type);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out int value, int size);
}
