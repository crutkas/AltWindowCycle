# AltWindowCycle

A tiny Windows utility that mimics the macOS **⌘+`** behavior: cycle between the
top-level windows of the **currently focused application**, without switching to
a different app.

| Shortcut | Action |
| --- | --- |
| <kbd>Alt</kbd>+<kbd>`</kbd> | Next window of the same app |
| <kbd>Shift</kbd>+<kbd>Alt</kbd>+<kbd>`</kbd> | Previous window of the same app |

This repo contains **two proof-of-concept implementations** (C++ and C#) so we can
compare size, footprint, and complexity before deciding what goes into the
PowerToys integration (phase 2).

> Note: <kbd>`</kbd> is the backtick/tilde key (`VK_OEM_3`) on US layouts.

## How it works

Both POCs share the same algorithm:

1. Get the foreground window and the full image path of its owning process.
2. Enumerate every top-level **Alt+Tab–eligible** window
   (visible, not DWM-cloaked, root-owner == self, not a tool window) whose
   process image path matches the foreground app.
3. Sort the group by `HWND` so the order is stable across activations
   (important: a Z-order based cycle would just ping-pong between two windows).
4. Find the foreground window's index and move forward/backward with wraparound.
5. Force-activate the target window
   (`AttachThreadInput` + `BringWindowToTop` + `SetForegroundWindow`).

"Same application" is defined as **same process image path**, which correctly
groups multi-window apps (and multi-process apps like browsers, whose top-level
windows belong to the main process).

## Layout

```
cpp/      Native Win32 POC (no dependencies)
csharp/   .NET 10 POC (P/Invoke, message-only window)
```

## Building

### C++

```powershell
.\cpp\build.ps1
# -> cpp\out\AltWindowCycle.exe
```

Static CRT (`/MT`), size-optimized (`/O1 /Os /GL /LTCG /OPT:REF,ICF`),
`/SUBSYSTEM:WINDOWS` (no console). Drop-and-run, no runtime required.

### C#

```powershell
.\csharp\build.ps1
# -> csharp\out\fdd\AltWindowCycle.exe  (framework-dependent, needs .NET 10)
# -> csharp\out\aot\AltWindowCycle.exe  (Native AOT, self-contained)
```

## POC comparison

Measured on Windows 11 (x64), VS 2026 / .NET 10:

| Build | Size | Self-contained | Notes |
| --- | ---: | --- | --- |
| C++ (`/MT`) | ~118 KB | ✅ | Smallest, zero deps, instant start |
| C# framework-dependent | ~158 KB | ❌ | Needs .NET 10 runtime installed |
| C# Native AOT | ~1.05 MB | ✅ | Self-contained, no runtime needed |

Takeaways:

- **C++** wins on size and footprint for a standalone tray-less utility.
- **C# AOT** is dramatically larger as a standalone exe but is far more
  maintainable and matches how most PowerToys modules are authored
  (the runtime is shared once PowerToys is installed, so the marginal cost is
  much smaller than the standalone 1 MB suggests).
- The core logic is identical and tiny in both; the decision for PowerToys is
  mostly about matching the existing module conventions.

## Running

Run either `AltWindowCycle.exe`. It registers the two global hotkeys and sits in
the background (no window, no tray icon — this is a POC). It is single-instance.
To stop it, end the `AltWindowCycle` process.

## Status

- [x] Phase 1: C++ and C# POCs with shared algorithm, built and validated
- [ ] Phase 2: PowerToys module integration (settings UI, configurable hotkey,
      enable/disable, telemetry, MRU ordering option)

## Possible future improvements

- MRU (most-recently-used) ordering instead of stable `HWND` order, matching
  macOS more closely (requires tracking activation order via a WinEvent hook).
- Configurable hotkey and "same app" definition (process path vs. AppUserModelID).
- Optional on-screen window picker overlay (à la Alt+Tab) while the key is held.
