# AltWindowCycle → PowerToys integration plan

Status: **Planning / arguing the approach.** No code written yet.

Goal: ship AltWindowCycle (cycle the windows of the *currently focused app* with
`Alt+\`` / `Shift+Alt+\``) as part of the PowerToys suite, reading its
configuration from a standard PowerToys `settings.json`.

Local PowerToys checkout for reference/work: `C:\Users\crutkas\source\repos\PowerToys`.

---

## TL;DR recommendation

**Implement AltWindowCycle as a Runner-resident feature** (C++ in `src/runner/`)
rather than a separate module process. The whole feature is "on a global hotkey,
re-activate the next/previous top-level window of the foreground app" — there is
no UI surface of its own, no long-lived state, and no background polling. That is
the lightest-weight shape PowerToys offers, and there is direct precedent for it
in the Runner today.

This reframes the earlier C#-vs-C++ footprint debate: **if it lives in the
Runner, there is no separate process and no .NET runtime at all** — it is a few
KB of C++ executing inside an already-running native process. Marginal cost is
effectively the hotkey registration plus the cycling routine.

The open question is not "C# or C++"; it is **how much module ceremony**
(enable/disable toggle, Settings UI page, GPO, telemetry) we want around an
otherwise trivial Runner-resident action. See "Decisions to make".

---

## Why this changes the earlier analysis

Earlier we compared standalone builds and a hypothetical .NET module:

| Build (standalone) | Disk marginal | Private bytes | Notes |
| --- | ---: | ---: | --- |
| C++ (`/MT`) | 118 KB | 0.90 MB | own process |
| C# framework-dependent | ~170 KB | 6.31 MB | own process, shared runtime pages |
| C# Native AOT | ~1.05 MB | 3.22 MB | own process, self-contained |

All of those assume **a separate process**. A Runner-resident feature has **no
new process**: the cost folds into `PowerToys.exe`, which is already running.
That dominates every other option on footprint.

---

## Precedent: features that live directly in the Runner

The Runner already owns the centralized hotkey infrastructure and executes
hotkey actions in-process. Some actions are wired up **directly in the Runner**,
not via a loaded module:

- `src/runner/tray_icon.cpp` registers the Quick Access / settings flyout hotkey
  inline:

  ```cpp
  hkmng.AddHotkey(hk, L"GeneralSettings", 0, true);
  CentralizedKeyboardHook::SetHotkeyAction(L"QuickAccess", hk, []() {
      open_quick_access_flyout_window();
      return true;
  });
  ```

- The Runner exposes a general-purpose hotkey-action API in
  `src/runner/centralized_hotkeys.h`:

  ```cpp
  namespace CentralizedHotkeys {
      struct Action   { std::wstring moduleName; std::function<void(WORD,WORD)> action; };
      struct Shortcut { WORD modifiersMask; WORD vkCode; int hotkeyID; };
      bool AddHotkeyAction(Shortcut shortcut, Action action);
      void UnregisterHotkeysForModule(std::wstring moduleName);
  }
  ```

  `AddHotkeyAction` calls `RegisterHotKey(runnerWindow, ...)` and dispatches the
  stored `std::function` when the shortcut fires
  (`src/runner/centralized_hotkeys.cpp`).

- For loaded modules, the same plumbing is reached via
  `src/runner/powertoy_module.cpp` (`get_hotkeys()` → `CentralizedHotkeys::AddHotkeyAction`
  / `CentralizedKeyboardHook::SetHotkeyAction`) and registers with the
  `HotkeyConflictDetector` so the Settings UI can warn about conflicts.

**Takeaway:** registering `Alt+\`` and `Shift+Alt+\`` with an inline C++ action
that performs the window cycle is exactly the shape of the existing QuickAccess
hotkey. The window-cycling logic is already written in C++ in
`cpp/AltWindowCycle.cpp` and ports directly.

---

## Architecture options

### Option A — Standard .NET module (like `src/modules/awake/`)
- `AltWindowCycle/` C# core exe + `AltWindowCycleModuleInterface/` C++ DLL +
  Settings UI page + `Settings.UI.Library` POCO.
- Settings read via `SettingsUtils.Default.GetSettings<T>("AltWindowCycle")`,
  live-reloaded with a `FileSystemWatcher` (see `src/modules/awake/Awake/Program.cs`).
- Pros: full conventions (toggle, Settings UI, GPO, telemetry, OOBE), isolation.
- Cons: heaviest — a separate .NET process (~6 MB private) for a trivial action.

### Option B — In-proc C++ module DLL
- A small `powertoy_create()` DLL loaded by the Runner, exposing `get_hotkeys()`.
- Runs inside the Runner process (no separate exe), integrates with conflict
  detection and the Settings UI hotkey picker via standard module plumbing.
- Pros: near-zero footprint, full module conventions, no IPC.
- Cons: still the full module/Settings-UI/installer ceremony to build out.

### Option C — Runner-resident feature (recommended)
- Register the two hotkeys directly in `src/runner/` (à la `tray_icon.cpp`),
  inline C++ action ports `cpp/AltWindowCycle.cpp`.
- Settings read by the Runner from `settings.json` (the Runner already reads
  settings, e.g. for the general-settings hotkey).
- Pros: lightest possible — no module, no process, no runtime; logic already
  exists in C++.
- Cons: bypasses the normal "feature = module" conventions; needs a deliberate
  plan for the enable/disable toggle, Settings UI surface, GPO, and telemetry
  (these are the things modules get "for free").

---

## The settings.json story

Confirmed requirement: configuration comes from a **standard PowerToys
`settings.json`** managed by the Settings UI.

- Location: `%LOCALAPPDATA%\Microsoft\PowerToys\AltWindowCycle\settings.json`.
- Shape: a POCO implementing `ISettingsConfig` with
  `GetModuleName() => "AltWindowCycle"`, surfaced by a Settings UI page.
- Proposed fields:
  - `NextWindowHotkey` : `HotkeySettings` (default `Alt+\``)
  - `PrevWindowHotkey` : `HotkeySettings` (default `Shift+Alt+\``)
  - `UseMruOrder` : bool (stable HWND order vs. most-recently-used)
  - `SameAppDefinition` : enum (`ProcessImagePath` vs. `AppUserModelID`)
- Who reads it:
  - Option A: the C# core, via `SettingsUtils` + `FileSystemWatcher`.
  - Options B/C: the Runner (C++) via `common/SettingsAPI` (`settings_objects.h`),
    re-reading on the Settings "refresh" signal and re-registering hotkeys.

Even in the Runner-resident option, the **Settings UI page is still C#** (that is
where users edit `settings.json`); only the *consumer* of the JSON is C++.

---

## Decisions to make

1. **Where does it live?** Runner-resident (C) vs in-proc module (B) vs .NET
   module (A). Leaning **C**, falling back to **B** if we want full conventions
   with minimal extra weight.
2. **Enable/disable:** if Runner-resident, how is the on/off toggle represented
   in General Settings / the module list? (Modules get this automatically.)
3. **Hotkey conflict detection:** wire into `HotkeyConflictDetector` so the
   Settings UI can warn on conflicts (modules get this via `get_hotkeys`).
4. **Settings UI page + GPO + telemetry + OOBE:** how much of this we build for a
   v1 vs. defer.
5. **Single source of truth for the cycling logic:** keep the C++ in
   `cpp/AltWindowCycle.cpp` as the canonical implementation to port into the
   Runner.

---

## Open research before committing

- Confirm whether any *user-toggleable* feature (not just core Runner hotkeys
  like QuickAccess) is implemented Runner-resident, or whether all toggleable
  features are modules. This decides whether C is idiomatic or a new pattern.
- Map how the Runner reads/refreshes a per-feature `settings.json` outside the
  module `get_config`/`set_config` path.
- Identify the minimum Settings UI surface to expose a hotkey picker + toggle.

---

## References (microsoft/PowerToys)

- `src/runner/centralized_hotkeys.{h,cpp}` — `AddHotkeyAction` API + `RegisterHotKey`.
- `src/runner/centralized_kb_hook.{h,cpp}` — low-level hook + `SetHotkeyAction`.
- `src/runner/tray_icon.cpp` — Runner-resident hotkey action precedent (QuickAccess).
- `src/runner/powertoy_module.cpp` — module `get_hotkeys()` → centralized hotkeys
  + `HotkeyConflictDetector`.
- `src/runner/hotkey_conflict_detector.{h,cpp}` — conflict detection.
- `src/modules/awake/` — reference .NET module (core exe + module interface +
  `Program.cs` settings read + `FileSystemWatcher`).
