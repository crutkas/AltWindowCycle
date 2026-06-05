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

## Alt-Tab-style overlay (C++ POC)

The C++ POC has an optional on-screen picker that shows window previews of the
candidate windows while you cycle — the same hold-to-preview feel as Alt+Tab:

1. **Tap** <kbd>Alt</kbd>+<kbd>`</kbd> and release quickly → instant switch, the
   overlay never appears (preserves the snappy default behavior).
2. **Hold** <kbd>Alt</kbd> and keep tapping <kbd>`</kbd> → after a short delay
   (~180 ms) the overlay appears; each tap advances the highlighted selection.
   <kbd>Shift</kbd>+<kbd>Alt</kbd>+<kbd>`</kbd> moves backward.
3. **Release** <kbd>Alt</kbd> to activate the highlighted window; **Esc** cancels.

How it's built (all raw Win32, so it ports straight into the C++ PowerToys module):

- A delayed-show state machine (`Idle → Pending → Visible`) captures the candidate
  list once on the first press and only materializes the overlay if you keep
  <kbd>Alt</kbd> held — quick taps stay flash-free.
- A non-layered `WS_POPUP` (`WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`,
  shown with `SW_SHOWNA`) carries an Alt-Tab-style "In-App Acrylic - Thin" backdrop
  without stealing focus. The POC uses the shell recipe values where Win32 exposes
  them: dark tint color `#545454`, zero tint opacity, 1px surface stroke, and 8px
  panel radius.
- Preview snapshots are drawn into the same per-pixel-alpha layered surface as the
  chrome, so rounded thumbnail corners are a real alpha mask over the acrylic
  background instead of opaque cover pixels. Each tile is a Win11-style card: a
  header "tab" on top with the **app icon + window title**, the preview below, and
  an accent outline on the selected tile. Per-monitor DPI aware, centered on the
  focused window's monitor work area.

The overlay is **on by default**; pass `--no-overlay` for the original
instant-switch behavior. In PowerToys this maps to a togglable setting.

`--selftest` force-shows the overlay for the app with the most open windows for a
few seconds (used to validate rendering headlessly).

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
# -> csharp\out\fdd\AltWindowCycle.exe       (framework-dependent, needs .NET 10)
# -> csharp\out\aot\AltWindowCycle.exe       (Native AOT, self-contained)
# -> csharp\out\aot-lzma\AltWindowCycle.exe  (Native AOT + LZMA compression)
```

LZMA compression is opt-in via `-p:AotCompress=true` (uses
[PublishAotCompressed](https://github.com/MichalStrehovsky/PublishAotCompressed)),
the same self-extracting trick PeekDesktop uses for its smallest builds.

## POC comparison

Measured on Windows 11 (x64), VS 2026 / .NET 10:

| Build | Size | Self-contained | Notes |
| --- | ---: | --- | --- |
| C++ (`/MT`) | ~128 KB | ✅ | Smallest, zero deps, instant start (incl. overlay) |
| C# framework-dependent | ~158 KB | ❌ | Needs .NET 10 runtime installed |
| C# Native AOT | ~1.05 MB | ✅ | Self-contained, no runtime needed |
| C# Native AOT + LZMA | ~471 KB | ✅ | Self-extracting; ~1 MB working set |

Takeaways:

- **C++** wins on size and footprint for a standalone tray-less utility — native
  code has no runtime floor at all (~1 MB idle working set).
- **C# Native AOT** sits on the ~1 MB AOT runtime floor (GC, type system,
  exception handling). Our app is so small it's basically *at* that floor, so the
  size knobs (`OptimizationPreference=Size`, `InvariantGlobalization`,
  `StackTraceSupport=false`, `UseSystemResourceKeys`) buy little here.
- **LZMA compression** (the trick behind PeekDesktop's headline "tiny" number)
  is a post-build self-extracting wrapper, *not* an AOT compiler setting. It
  roughly halves the on-disk size (~471 KB) at the cost of a small decompress on
  startup. PeekDesktop's own uncompressed AOT was ~1.88 MB — larger than ours —
  because it does much more (tray icon, JSON settings, update check).
- The core logic is identical and tiny in both languages; the PowerToys decision
  is mostly about matching existing module conventions (the .NET runtime is
  shared once PowerToys is installed, so AOT's standalone 1 MB is not a real cost
  there).

## Running

Run either `AltWindowCycle.exe`. It registers the two global hotkeys and sits in
the background (no tray icon — this is a POC). It is single-instance. To stop it,
end the `AltWindowCycle` process.

The C++ build shows the Alt-Tab-style overlay by default (see above); run it with
`--no-overlay` for instant switching, or `--selftest` to preview the overlay.

## Status

- [x] Phase 1: C++ and C# POCs with shared algorithm, built and validated
- [x] Alt-Tab-style acrylic preview overlay (C++ POC, togglable via `--no-overlay`)
- [ ] Port the overlay into the PowerToys C++ module as a togglable setting
- [ ] Phase 2: PowerToys module integration (settings UI, configurable hotkey,
      enable/disable, telemetry, MRU ordering option)

## Possible future improvements

- MRU (most-recently-used) ordering instead of stable `HWND` order, matching
  macOS more closely (requires tracking activation order via a WinEvent hook).
- Configurable hotkey and "same app" definition (process path vs. AppUserModelID).
- Port the acrylic preview overlay (currently C++ only) to the C# POC, or straight
  into the PowerToys C++ module behind a `show_overlay` setting.
- Low-level keyboard hook for Alt-release detection (the POC polls
  `GetAsyncKeyState` on a timer, which is fine but slightly less precise).
