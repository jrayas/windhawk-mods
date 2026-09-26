# Dynamic Active Taskbar App Alignment

**Windhawk mod** · Windows 11 (22H2+) · x86-64

Slides the focused app's taskbar button to the right end of the button strip,
snapping back when the desktop is focused.

---

## What it does

- Watches for foreground-window changes via a `WinEventHook`.
- Finds the taskbar button that is currently in its **Active** visual state.
- Applies a `TranslateTransform` that slides that button to the right end of the
  button strip and shifts every button after it left to close the gap.
- Clears all transforms when the desktop (or the taskbar itself) is focused.
- Animates the transition with a configurable cubic-ease duration.

---

## Settings

| Setting | Default | Description |
|---|---|---|
| `animationMs` | `180` | Slide duration in milliseconds. `0` = instant snap. |
| `extraGap` | `0` | Extra pixels between the active button and the right edge. |
| `restoreOnDesktop` | `true` | Snap back when the desktop / Start / taskbar gains focus. |
| `includeSecondaryTaskbars` | `true` | Apply to secondary-monitor taskbars. |
| `settleDelayMs` | `90` | Delay (ms) after a focus change before reading visual states. Increase if the wrong button moves. |
| `symbolOverride` | _(empty)_ | Force a specific `TaskbarFrame` symbol instead of auto-pick. |
| `dumpSymbols` | `false` | Log every `TaskbarFrame` symbol found in `Taskbar.View.dll`. |
| `debugTree` | `false` | Dump the taskbar element tree and visual-state names to the log. |

---

## Requirements

- **Windows 11 22H2 or later** — the taskbar is XAML-based; earlier builds are not supported.
- [Windhawk](https://windhawk.net) installed and running.

---

## How it works

The mod enumerates `Taskbar.View.dll`'s symbols at startup and hooks up to
`kMaxHooks` (6) `TaskbarFrame` instance methods by address — no hard-coded
mangled names that go stale between builds. Whichever hooked method fires
first registers the frame element; duplicates are de-duplicated.

Layout is re-applied on every `LayoutUpdated` event from the button strip and
on every foreground-window change, using a signature hash to skip passes when
nothing has moved.

Offsets are written as `TranslateTransform.X` with `FillBehavior::Stop`
storyboards for animation, so the dependency property always reflects the final
value and storyboards never shadow it.

---

## Diagnostics

Check the Windhawk log:

| Log entry | Meaning |
|---|---|
| `Hooking (arity N): ...` + `TaskbarFrame registered` | Working correctly. |
| `No hookable TaskbarFrame method found` | Enable **Dump TaskbarFrame symbols to log**, reload, then force one via **Symbol override**. |

Enable **Dump visual tree to log** to print the element tree and visual-state
names your build actually uses, in case `TaskbarFrameRepeater` or the active
state name differs from the known variants.

---

## Author

Dom — version 2.0.0
