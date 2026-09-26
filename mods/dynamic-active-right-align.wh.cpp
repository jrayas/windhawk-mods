// ==WindhawkMod==
// @id              dynamic-active-right-align
// @name            Dynamic Active Taskbar App Alignment
// @description     Slides the focused app's taskbar button to the right end of the button strip, snapping back when the desktop is focused.
// @version         2.0.0
// @author          Dom
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lruntimeobject -lole32 -loleaut32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Dynamic Active Taskbar App Alignment

Windows 11 (22H2+) only. The taskbar is XAML, so this mod works on the XAML
visual tree rather than on window handles.

## What it does

* Watches for foreground-window changes.
* Finds the taskbar button that is currently in its "active" visual state.
* Applies a render transform that slides that button to the right end of the
  button strip, and shifts every button after it left to close the gap.
* Clears all transforms when the desktop (or the taskbar itself) is focused.

## If it does nothing

The mod enumerates `Taskbar.View.dll`'s symbols at startup and picks a
`TaskbarFrame` method to hook by address, so there are no hard-coded mangled
names to go stale. Check the log:

* `Hooking (arity N): ...` followed by `TaskbarFrame registered` - working.
* `No hookable TaskbarFrame method found` - turn on "Dump TaskbarFrame symbols
  to log", reload, and pick one by hand via "Symbol override".

Enable "Dump visual tree to log" to print the element tree and the visual-state
names your build actually uses, in case `TaskbarFrameRepeater` or the active
state name differ.

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- animationMs: 180
  $name: Animation duration (ms)
  $description: Time taken to slide buttons. 0 snaps instantly.
- extraGap: 0
  $name: Extra right margin (px)
  $description: Pixels of space to leave between the active button and the right edge of the strip.
- restoreOnDesktop: true
  $name: Snap back on desktop focus
  $description: Clear the offset when the desktop, Start, or the taskbar is focused.
- includeSecondaryTaskbars: true
  $name: Apply to secondary taskbars
- settleDelayMs: 90
  $name: Settle delay (ms)
  $description: How long to wait after a focus change before reading the taskbar's visual states. Raise this if the wrong button moves.
- symbolOverride: ""
  $name: Symbol override
  $description: Leave empty to auto-pick. To force one, paste the exact undecorated symbol name from the log.
- dumpSymbols: false
  $name: Dump TaskbarFrame symbols to log
  $description: Lists every TaskbarFrame symbol found in Taskbar.View.dll. Use this if auto-pick fails.
- debugTree: false
  $name: Dump visual tree to log
  $description: Prints the taskbar element tree and visual-state names. Use this to adapt the mod to your build.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

// windhawk_utils.h pulls in windows.h, and winbase.h defines GetCurrentTime()
// as a macro. winrt/Windows.UI.Xaml.Media.Animation.0.h declares
// IStoryboard::GetCurrentTime(int64_t*), which the macro then destroys.
// Must be undefined before any winrt Xaml header is included.
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using winrt::Windows::UI::Xaml::Controls::Primitives::LayoutInformation;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Media::Animation;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct {
    int animationMs;
    int extraGap;
    bool restoreOnDesktop;
    bool includeSecondaryTaskbars;
    int settleDelayMs;
    bool dumpSymbols;
    bool debugTree;
} g_settings;

static void LoadSettings() {
    g_settings.animationMs = Wh_GetIntSetting(L"animationMs");
    g_settings.extraGap = Wh_GetIntSetting(L"extraGap");
    g_settings.restoreOnDesktop = Wh_GetIntSetting(L"restoreOnDesktop") != 0;
    g_settings.includeSecondaryTaskbars =
        Wh_GetIntSetting(L"includeSecondaryTaskbars") != 0;
    g_settings.settleDelayMs = Wh_GetIntSetting(L"settleDelayMs");
    g_settings.dumpSymbols = Wh_GetIntSetting(L"dumpSymbols") != 0;
    g_settings.debugTree = Wh_GetIntSetting(L"debugTree") != 0;

    if (g_settings.animationMs < 0) g_settings.animationMs = 0;
    if (g_settings.animationMs > 2000) g_settings.animationMs = 2000;
    if (g_settings.settleDelayMs < 0) g_settings.settleDelayMs = 0;
    if (g_settings.settleDelayMs > 1000) g_settings.settleDelayMs = 1000;
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

// Every TaskbarFrame we have seen (primary + one per secondary taskbar).
static std::mutex g_framesMutex;
static std::vector<winrt::weak_ref<FrameworkElement>> g_frames;

// WinEvent hook lives on its own thread, which owns a message loop.
static HANDLE g_eventThread = nullptr;
static DWORD g_eventThreadId = 0;
static HWINEVENTHOOK g_winEventHook = nullptr;

// Set once dispatched callbacks must not run any more (DLL about to unmap).
static std::atomic<bool> g_unloading{false};
// Set earlier, to stop reacting to new foreground events during shutdown.
static std::atomic<bool> g_stopEvents{false};
// Refcount of in-flight dispatched callbacks, so uninit can wait them out.
static std::atomic<int> g_pendingCallbacks{0};

// ---------------------------------------------------------------------------
// Visual tree helpers
// ---------------------------------------------------------------------------

// State names that have meant "this button's window is in the foreground" on
// the builds seen so far. Checked case-insensitively against every visual
// state group on the button.
static PCWSTR kActiveStateNames[] = {
    L"Active",
    L"ForegroundActive",
    L"Foreground",
    L"Selected",
    L"ActiveRunningIndicator",
};

static bool EqualsNoCase(std::wstring_view a, PCWSTR b) {
    return _wcsicmp(std::wstring(a).c_str(), b) == 0;
}

static std::vector<FrameworkElement> GetChildElements(const DependencyObject& parent) {
    std::vector<FrameworkElement> out;
    if (!parent) {
        return out;
    }
    int count = VisualTreeHelper::GetChildrenCount(parent);
    out.reserve(count);
    for (int i = 0; i < count; i++) {
        auto child = VisualTreeHelper::GetChild(parent, i);
        if (auto fe = child.try_as<FrameworkElement>()) {
            out.push_back(fe);
        }
    }
    return out;
}

// Depth-first search for a descendant whose Name() matches, with an optional
// type-name match as a fallback (element names differ across builds).
static FrameworkElement FindDescendant(const FrameworkElement& root,
                                       PCWSTR name,
                                       PCWSTR typeName,
                                       int maxDepth = 12) {
    if (!root || maxDepth < 0) {
        return nullptr;
    }
    for (const auto& child : GetChildElements(root)) {
        if (name && !child.Name().empty() && EqualsNoCase(child.Name(), name)) {
            return child;
        }
        if (typeName) {
            // Keep the hstring alive; a wstring_view bound straight to the
            // temporary would dangle.
            winrt::hstring classNameHolder = winrt::get_class_name(child);
            std::wstring_view className{classNameHolder};
            if (className.find(typeName) != std::wstring_view::npos) {
                return child;
            }
        }
        if (auto found = FindDescendant(child, name, typeName, maxDepth - 1)) {
            return found;
        }
    }
    return nullptr;
}

static void DumpTree(const FrameworkElement& element, int depth = 0) {
    if (!element || depth > 10) {
        return;
    }
    std::wstring indent(depth * 2, L' ');
    auto className = winrt::get_class_name(element);
    Wh_Log(L"%s%s  name=\"%s\"  w=%.1f h=%.1f", indent.c_str(), className.c_str(),
           element.Name().c_str(), element.ActualWidth(), element.ActualHeight());

    auto groups = VisualStateManager::GetVisualStateGroups(element);
    if (groups && groups.Size() > 0) {
        for (uint32_t i = 0; i < groups.Size(); i++) {
            auto group = groups.GetAt(i);
            auto current = group.CurrentState();
            Wh_Log(L"%s  [state] group=\"%s\" current=\"%s\"", indent.c_str(),
                   group.Name().c_str(),
                   current ? current.Name().c_str() : L"(none)");
        }
    }

    for (const auto& child : GetChildElements(element)) {
        DumpTree(child, depth + 1);
    }
}

// Walks the element and its descendants looking for an active visual state.
static bool IsButtonActive(const FrameworkElement& button, int depth = 4) {
    if (!button || depth < 0) {
        return false;
    }
    auto groups = VisualStateManager::GetVisualStateGroups(button);
    if (groups) {
        for (uint32_t i = 0; i < groups.Size(); i++) {
            auto current = groups.GetAt(i).CurrentState();
            if (!current) {
                continue;
            }
            for (PCWSTR candidate : kActiveStateNames) {
                if (EqualsNoCase(current.Name(), candidate)) {
                    return true;
                }
            }
        }
    }
    for (const auto& child : GetChildElements(button)) {
        if (IsButtonActive(child, depth - 1)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

struct ButtonBox {
    FrameworkElement element;
    double x = 0.0;       // layout-slot X, never includes our render transform
    double width = 0.0;
};

// Per-taskbar bookkeeping. Every field is only touched on that taskbar's own
// UI thread except for the list itself, which is guarded.
struct FrameState {
    void* key = nullptr;
    bool layoutHooked = false;
    double signature = -1.0;
    bool lastRestoreOnly = true;
    // Elements we have given a transform to, so stale ones can be reset even
    // after the repeater recycles them out of the visual tree.
    std::vector<winrt::weak_ref<FrameworkElement>> touched;
};

static std::recursive_mutex g_stateMutex;
static std::deque<FrameState> g_frameStates;

static FrameState& GetFrameState(const FrameworkElement& frame) {
    void* key = winrt::get_abi(frame);
    for (auto& state : g_frameStates) {
        if (state.key == key) {
            return state;
        }
    }
    g_frameStates.push_back(FrameState{key});
    return g_frameStates.back();
}

static TranslateTransform EnsureTranslateTransform(const FrameworkElement& element) {
    if (auto translate = element.RenderTransform().try_as<TranslateTransform>()) {
        return translate;
    }
    TranslateTransform translate;
    element.RenderTransform(translate);
    return translate;
}

// The dependency property is always set to the final value immediately, so it
// is the single source of truth for "offset currently applied". The storyboard
// only interpolates visually, and uses FillBehavior::Stop so it never holds a
// value that would shadow the property afterwards.
static void SetOffset(const FrameworkElement& element, double target) {
    auto translate = EnsureTranslateTransform(element);
    double from = translate.X();

    if (std::fabs(from - target) < 0.5) {
        translate.X(target);
        return;
    }

    translate.X(target);

    if (g_settings.animationMs <= 0) {
        return;
    }

    DoubleAnimation animation;
    animation.From(from);
    animation.To(target);
    animation.Duration(winrt::Windows::UI::Xaml::Duration{
        std::chrono::milliseconds(g_settings.animationMs)});
    animation.FillBehavior(FillBehavior::Stop);
    animation.EnableDependentAnimation(true);

    CubicEase ease;
    ease.EasingMode(EasingMode::EaseOut);
    animation.EasingFunction(ease);

    Storyboard storyboard;
    storyboard.Children().Append(animation);
    Storyboard::SetTarget(animation, translate);
    Storyboard::SetTargetProperty(animation, L"X");
    storyboard.Begin();
}

// Returns the ItemsRepeater (or equivalent panel) holding the task buttons.
static FrameworkElement GetButtonStrip(const FrameworkElement& frame) {
    if (auto strip = FindDescendant(frame, L"TaskbarFrameRepeater", nullptr)) {
        return strip;
    }
    if (auto strip = FindDescendant(frame, nullptr, L"ItemsRepeater")) {
        return strip;
    }
    if (auto strip = FindDescendant(frame, L"TaskbarElements", nullptr)) {
        return strip;
    }
    return nullptr;
}

// Measures from the arrange slot, which is assigned by the parent before any
// render transform is applied. Reading TransformToVisual instead would feed our
// own offsets back into the next calculation.
static std::vector<ButtonBox> MeasureButtons(const FrameworkElement& strip,
                                             std::vector<FrameworkElement>& allChildren) {
    std::vector<ButtonBox> boxes;
    allChildren = GetChildElements(strip);

    for (const auto& child : allChildren) {
        if (child.Visibility() != Visibility::Visible) {
            continue;
        }
        auto slot = LayoutInformation::GetLayoutSlot(child);
        if (slot.Width <= 1.0f) {
            // Not arranged yet (realizing or recycled) - excluded from the
            // calculation, but still reset below so it cannot keep a stale
            // offset from whatever item it used to display.
            continue;
        }
        boxes.push_back({child, slot.X, slot.Width});
    }

    std::sort(boxes.begin(), boxes.end(),
              [](const ButtonBox& a, const ButtonBox& b) { return a.x < b.x; });
    return boxes;
}

static bool IsSameElement(const FrameworkElement& a, const FrameworkElement& b) {
    return winrt::get_abi(a) == winrt::get_abi(b);
}

// Zero out anything we previously moved that is no longer a participating
// child. Done instantly rather than animated - these elements are off-tree or
// unarranged, so there is nothing to see.
static void ResetStale(FrameState& state, const std::vector<ButtonBox>& boxes) {
    for (auto& weak : state.touched) {
        auto element = weak.get();
        if (!element) {
            continue;
        }
        bool participating = false;
        for (const auto& box : boxes) {
            if (IsSameElement(box.element, element)) {
                participating = true;
                break;
            }
        }
        if (!participating) {
            if (auto translate = element.RenderTransform().try_as<TranslateTransform>()) {
                translate.X(0.0);
            }
        }
    }
    state.touched.clear();
}

static double ComputeSignature(const std::vector<ButtonBox>& boxes) {
    double signature = static_cast<double>(boxes.size()) * 7919.0;
    for (const auto& box : boxes) {
        signature = signature * 31.0 + box.x * 3.0 + box.width;
    }
    return signature;
}

static void ApplyLayout(const FrameworkElement& frame, bool restoreOnly,
                        bool fromLayoutUpdated = false);

static void HookLayoutUpdated(const FrameworkElement& frame,
                              const FrameworkElement& strip, FrameState& state) {
    if (state.layoutHooked) {
        return;
    }
    state.layoutHooked = true;

    auto weakFrame = winrt::make_weak(frame);
    strip.LayoutUpdated([weakFrame](auto&&, auto&&) {
        if (g_unloading.load()) {
            return;
        }
        auto frame = weakFrame.get();
        if (!frame) {
            return;
        }
        bool restoreOnly;
        {
            std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
            restoreOnly = GetFrameState(frame).lastRestoreOnly;
        }
        // Re-applies only when the underlying layout actually changed; the
        // signature check inside makes this a no-op otherwise.
        ApplyLayout(frame, restoreOnly, true);
    });
}

// Core layout: slide the active button to the right end, pull the rest left.
// Idempotent by construction - running it twice with the same layout produces
// the same offsets, so repeated calls cannot accumulate drift.
static void ApplyLayout(const FrameworkElement& frame, bool restoreOnly,
                        bool fromLayoutUpdated) {
    auto strip = GetButtonStrip(frame);
    if (!strip) {
        Wh_Log(L"Button strip not found under TaskbarFrame");
        return;
    }

    if (g_settings.debugTree && !fromLayoutUpdated) {
        Wh_Log(L"---- taskbar visual tree ----");
        DumpTree(frame);
        Wh_Log(L"---- end tree ----");
    }

    std::vector<FrameworkElement> allChildren;
    auto boxes = MeasureButtons(strip, allChildren);

    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    FrameState& state = GetFrameState(frame);

    HookLayoutUpdated(frame, strip, state);

    double signature = ComputeSignature(boxes);
    if (fromLayoutUpdated && signature == state.signature) {
        return;  // nothing moved; avoid churning on every layout pass
    }
    state.signature = signature;
    state.lastRestoreOnly = restoreOnly;

    ResetStale(state, boxes);

    auto applyAll = [&](double value) {
        for (const auto& box : boxes) {
            SetOffset(box.element, value);
            state.touched.push_back(winrt::make_weak(box.element));
        }
    };

    if (restoreOnly || boxes.size() < 2) {
        applyAll(0.0);
        return;
    }

    int activeIndex = -1;
    for (size_t i = 0; i < boxes.size(); i++) {
        if (IsButtonActive(boxes[i].element)) {
            activeIndex = static_cast<int>(i);
            break;
        }
    }

    // Nothing marked active, or it is already last: flat layout.
    if (activeIndex < 0 || activeIndex == static_cast<int>(boxes.size()) - 1) {
        applyAll(0.0);
        return;
    }

    const ButtonBox& active = boxes[activeIndex];
    const ButtonBox& last = boxes.back();
    double rightEdge = last.x + last.width;

    double gap = boxes[1].x - (boxes[0].x + boxes[0].width);
    if (gap < 0.0 || gap > 64.0) {
        gap = 0.0;  // non-uniform layout; do not invent spacing
    }

    double shiftLeft = active.width + gap;
    double activeTarget = rightEdge - active.width - g_settings.extraGap;

    // Everything must stay inside the strip. Without this clamp a bad
    // measurement pushes buttons past the left edge and they vanish.
    double stripWidth = strip.ActualWidth();
    double bound = (std::max)(stripWidth, rightEdge);

    for (size_t i = 0; i < boxes.size(); i++) {
        const ButtonBox& box = boxes[i];
        double offset;
        if (static_cast<int>(i) == activeIndex) {
            offset = activeTarget - box.x;
        } else if (static_cast<int>(i) > activeIndex) {
            offset = -shiftLeft;
        } else {
            offset = 0.0;
        }

        double left = box.x + offset;
        if (left < 0.0) {
            offset = -box.x;
        } else if (left + box.width > bound) {
            offset = bound - box.width - box.x;
        }

        SetOffset(box.element, offset);
        state.touched.push_back(winrt::make_weak(box.element));
    }
}

// ---------------------------------------------------------------------------
// Dispatching work onto each taskbar's UI thread
// ---------------------------------------------------------------------------

static void ScheduleLayout(bool restoreOnly) {
    std::vector<winrt::weak_ref<FrameworkElement>> frames;
    {
        std::lock_guard<std::mutex> lock(g_framesMutex);
        frames = g_frames;
    }

    for (auto& weak : frames) {
        auto frame = weak.get();
        if (!frame) {
            continue;
        }

        auto dispatcher = frame.Dispatcher();
        if (!dispatcher) {
            continue;
        }

        g_pendingCallbacks++;
        auto action = dispatcher.RunAsync(
            winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
            [weak, restoreOnly]() {
                if (g_unloading.load()) {
                    g_pendingCallbacks--;
                    return;
                }
                auto frame = weak.get();
                if (!frame) {
                    g_pendingCallbacks--;
                    return;
                }

                // Give the taskbar a moment to update its own visual states
                // before we read them.
                if (g_settings.settleDelayMs > 0 && !restoreOnly) {
                    DispatcherTimer timer;
                    timer.Interval(std::chrono::milliseconds(g_settings.settleDelayMs));
                    auto token = std::make_shared<winrt::event_token>();
                    *token = timer.Tick([weak, timer, token](auto&&, auto&&) mutable {
                        timer.Stop();
                        timer.Tick(*token);
                        if (!g_unloading.load()) {
                            if (auto frame = weak.get()) {
                                try {
                                    ApplyLayout(frame, false);
                                } catch (const winrt::hresult_error& e) {
                                    Wh_Log(L"ApplyLayout failed: %s", e.message().c_str());
                                }
                            }
                        }
                        g_pendingCallbacks--;
                    });
                    timer.Start();
                    return;
                }

                try {
                    ApplyLayout(frame, restoreOnly);
                } catch (const winrt::hresult_error& e) {
                    Wh_Log(L"ApplyLayout failed: %s", e.message().c_str());
                }
                g_pendingCallbacks--;
            });
        (void)action;
    }
}

// ---------------------------------------------------------------------------
// Foreground tracking
// ---------------------------------------------------------------------------

static bool IsDesktopOrShellWindow(HWND hwnd) {
    WCHAR className[128] = {};
    int len = GetClassNameW(hwnd, className, ARRAYSIZE(className));
    if (len <= 0) {
        return false;
    }

    static PCWSTR kShellClasses[] = {
        L"Progman",
        L"WorkerW",
        L"SHELLDLL_DefView",
        L"SysListView32",
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow",  // Start / Search flyouts
    };

    for (PCWSTR candidate : kShellClasses) {
        if (wcscmp(className, candidate) == 0) {
            return true;
        }
    }
    return false;
}

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                  LONG idChild, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW ||
        idChild != CHILDID_SELF || !hwnd) {
        return;
    }
    if (g_unloading.load() || g_stopEvents.load()) {
        return;
    }

    // Only top-level windows.
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root != hwnd) {
        return;
    }
    // Ignore tool windows and other non-taskbar-worthy windows.
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) {
        return;
    }

    bool restore = IsDesktopOrShellWindow(hwnd);
    if (restore && !g_settings.restoreOnDesktop) {
        return;
    }

    ScheduleLayout(restore);
}

static DWORD WINAPI EventThreadProc(LPVOID) {
    // Force a message queue to exist before anyone posts to this thread.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    g_winEventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                     nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!g_winEventHook) {
        Wh_Log(L"SetWinEventHook failed: %u", GetLastError());
        return 1;
    }

    // WINEVENT_OUTOFCONTEXT delivers callbacks through this thread's message
    // queue, so the loop below is what makes the hook fire at all.
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) {
            break;
        }
        if (msg.message == WM_QUIT) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWinEvent(g_winEventHook);
    g_winEventHook = nullptr;
    return 0;
}

// ---------------------------------------------------------------------------
// Symbol discovery - capture the TaskbarFrame XAML element
// ---------------------------------------------------------------------------
//
// Rather than hard-coding mangled names that change between builds, the mod
// enumerates Taskbar.View.dll's symbols at runtime, picks a suitable
// TaskbarFrame instance method, and hooks it by address.

static void CaptureFrame(void* pThis);

// Several candidates are hooked at once: whichever fires first registers the
// frame, and RegisterFrame de-duplicates. This avoids depending on any single
// method being called at a useful time - OnLoaded, for instance, has already
// fired by the time the mod is injected into a running explorer.
constexpr size_t kMaxHooks = 6;

using TaskbarFrame_Arity0_t = void(WINAPI*)(void* pThis);
using TaskbarFrame_Arity2_t = void(WINAPI*)(void* pThis, void* a1, void* a2);

template <size_t N>
struct HookSlot {
    static inline TaskbarFrame_Arity0_t orig0 = nullptr;
    static inline TaskbarFrame_Arity2_t orig2 = nullptr;

    static void WINAPI Hook0(void* pThis) {
        orig0(pThis);
        CaptureFrame(pThis);
    }
    static void WINAPI Hook2(void* pThis, void* a1, void* a2) {
        orig2(pThis, a1, a2);
        CaptureFrame(pThis);
    }
};

template <size_t... I>
static auto MakeHookTable(std::index_sequence<I...>) {
    struct Table {
        std::array<void*, sizeof...(I)> hook0;
        std::array<void*, sizeof...(I)> hook2;
        std::array<void**, sizeof...(I)> orig0;
        std::array<void**, sizeof...(I)> orig2;
    };
    return Table{
        {reinterpret_cast<void*>(&HookSlot<I>::Hook0)...},
        {reinterpret_cast<void*>(&HookSlot<I>::Hook2)...},
        {reinterpret_cast<void**>(&HookSlot<I>::orig0)...},
        {reinterpret_cast<void**>(&HookSlot<I>::orig2)...},
    };
}

static const auto g_hookTable = MakeHookTable(std::make_index_sequence<kMaxHooks>{});

// The implementation object's layout differs between builds; try a few
// candidate interface slots and keep whichever QIs to IFrameworkElement.
// Windhawk builds mods with clang, and SEH is avoided here - the slots are
// validated with VirtualQuery instead.
static bool IsReadablePointer(const void* p) {
    if (!p) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable)) {
        return false;
    }
    return !(mbi.Protect & PAGE_GUARD);
}

static FrameworkElement FrameworkElementFromImpl(void* pThis) {
    if (!IsReadablePointer(pThis)) {
        return nullptr;
    }

    const auto iid = winrt::guid_of<FrameworkElement>();

    auto tryQuery = [&iid](void* candidate) -> FrameworkElement {
        if (!IsReadablePointer(candidate)) {
            return nullptr;
        }
        // A COM interface pointer's first field is a readable vtable pointer.
        void* vtable = *reinterpret_cast<void**>(candidate);
        if (!IsReadablePointer(vtable)) {
            return nullptr;
        }
        void* abi = nullptr;
        HRESULT hr = reinterpret_cast<::IUnknown*>(candidate)->QueryInterface(
            reinterpret_cast<const GUID&>(iid), &abi);
        if (FAILED(hr) || !abi) {
            return nullptr;
        }
        FrameworkElement element = nullptr;
        winrt::attach_abi(element, abi);
        return element;
    };

    // Some builds hand us the projected interface directly.
    if (auto element = tryQuery(pThis)) {
        return element;
    }
    // Others give the implementation object, whose bases sit in the first slots.
    for (int slot = 1; slot < 4; slot++) {
        void* candidate = reinterpret_cast<void**>(pThis)[slot];
        if (auto element = tryQuery(candidate)) {
            return element;
        }
    }
    return nullptr;
}

static void RegisterFrame(const FrameworkElement& frame) {
    if (!frame) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_framesMutex);

    if (!g_settings.includeSecondaryTaskbars && !g_frames.empty()) {
        // Primary taskbar only: the primary frame is the first one we see.
        return;
    }

    // Drop dead entries and avoid duplicates.
    for (auto it = g_frames.begin(); it != g_frames.end();) {
        auto existing = it->get();
        if (!existing) {
            it = g_frames.erase(it);
            continue;
        }
        if (existing == frame) {
            return;
        }
        ++it;
    }
    g_frames.push_back(winrt::make_weak(frame));
    Wh_Log(L"TaskbarFrame registered (%zu total)", g_frames.size());
}

static void CaptureFrame(void* pThis) {
    if (g_unloading.load()) {
        return;
    }
    try {
        if (auto frame = FrameworkElementFromImpl(pThis)) {
            RegisterFrame(frame);
        }
    } catch (const winrt::hresult_error& e) {
        Wh_Log(L"Failed to capture TaskbarFrame: %s", e.message().c_str());
    }
}

// --- candidate selection ---------------------------------------------------

struct SymbolCandidate {
    std::wstring name;
    void* address = nullptr;
    int arity = -1;
    int score = 0;
};

static bool Contains(const std::wstring& haystack, PCWSTR needle) {
    return haystack.find(needle) != std::wstring::npos;
}

static bool EndsWith(const std::wstring& haystack, PCWSTR needle) {
    size_t len = wcslen(needle);
    return haystack.size() >= len &&
           haystack.compare(haystack.size() - len, len, needle) == 0;
}

// Returns the parameter count we can safely forward, or -1 if the shape is
// one this mod does not know how to call through.
static int ClassifyArity(const std::wstring& name) {
    if (EndsWith(name, L"(void)")) {
        return 0;
    }
    // The common XAML event-handler shape.
    if (Contains(name, L"IInspectable const &") &&
        (Contains(name, L"EventArgs const &") || Contains(name, L"IRoutedEventArgs"))) {
        return 2;
    }
    return -1;
}

static int ScoreCandidate(const std::wstring& name, int arity) {
    int score;
    // Methods that run on every layout or state change are what we want. A
    // one-shot lifecycle event is nearly useless when the mod is injected into
    // an already-running explorer, so those score below unknown methods.
    if (Contains(name, L"UpdateFrameHeight")) score = 50;
    else if (Contains(name, L"OnSizeChanged")) score = 46;
    else if (Contains(name, L"UpdateVisualStates")) score = 44;
    else if (Contains(name, L"OnPointer")) score = 40;
    else if (Contains(name, L"Layout")) score = 38;
    else if (Contains(name, L"Update")) score = 36;
    else if (Contains(name, L"Changed")) score = 34;
    else if (Contains(name, L"OnApplyTemplate")) score = 20;
    else if (Contains(name, L"OnLoaded")) score = 12;   // one-shot
    else if (Contains(name, L"Initialize") || Contains(name, L"::Init")) score = 8;
    else score = 25;

    // Arity 0 needs no argument forwarding, so slightly safer.
    if (arity == 0) score += 3;
    return score;
}

static bool IsUnsuitable(const std::wstring& name) {
    return Contains(name, L"~") ||                       // destructors
           Contains(name, L"`") ||                       // compiler thunks
           Contains(name, L"operator") ||
           Contains(name, L"static ") ||
           Contains(name, L"TaskbarFrame::TaskbarFrame"); // constructors
}

static HMODULE GetTaskbarViewModule() {
    if (HMODULE module = GetModuleHandleW(L"Taskbar.View.dll")) {
        return module;
    }

    WCHAR windowsDir[MAX_PATH];
    if (!GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir))) {
        return nullptr;
    }

    WCHAR fullPath[MAX_PATH * 2];
    swprintf(fullPath, ARRAYSIZE(fullPath),
             LR"(%s\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\Taskbar.View.dll)",
             windowsDir);

    return LoadLibraryExW(fullPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static bool HookTaskbarView() {
    HMODULE module = GetTaskbarViewModule();
    if (!module) {
        Wh_Log(L"Taskbar.View.dll not found - is this Windows 11 22H2 or later?");
        return false;
    }

    PCWSTR overrideSetting = Wh_GetStringSetting(L"symbolOverride");
    std::wstring symbolOverride = overrideSetting ? overrideSetting : L"";
    if (overrideSetting) {
        Wh_FreeStringSetting(overrideSetting);
    }

    std::vector<SymbolCandidate> candidates;
    int totalMatched = 0;

    WH_FIND_SYMBOL findSymbol;
    HANDLE findHandle = Wh_FindFirstSymbol(module, nullptr, &findSymbol);
    if (!findHandle) {
        Wh_Log(L"Wh_FindFirstSymbol failed - symbols could not be loaded");
        return false;
    }

    do {
        if (!findSymbol.symbol || !findSymbol.address) {
            continue;
        }
        std::wstring name = findSymbol.symbol;
        if (!Contains(name, L"TaskbarFrame::")) {
            continue;
        }
        totalMatched++;

        if (g_settings.dumpSymbols) {
            Wh_Log(L"[symbol] %s", name.c_str());
        }

        if (!symbolOverride.empty()) {
            if (name == symbolOverride) {
                int arity = ClassifyArity(name);
                candidates.push_back({name, findSymbol.address,
                                      arity < 0 ? 0 : arity, 1000});
            }
            continue;
        }

        if (IsUnsuitable(name)) {
            continue;
        }
        int arity = ClassifyArity(name);
        if (arity < 0) {
            continue;
        }
        candidates.push_back({name, findSymbol.address, arity,
                              ScoreCandidate(name, arity)});
    } while (Wh_FindNextSymbol(findHandle, &findSymbol));

    Wh_FindCloseSymbol(findHandle);

    Wh_Log(L"Found %d TaskbarFrame symbols, %zu usable", totalMatched,
           candidates.size());

    if (candidates.empty()) {
        Wh_Log(L"No hookable TaskbarFrame method found.");
        Wh_Log(L"Turn on 'Dump TaskbarFrame symbols to log', reload, and send the list.");
        return false;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const SymbolCandidate& a, const SymbolCandidate& b) {
                  return a.score > b.score;
              });

    // Log the shortlist so a bad auto-pick can be diagnosed from the log alone.
    size_t shown = (std::min)(candidates.size(), (size_t)12);
    for (size_t i = 0; i < shown; i++) {
        Wh_Log(L"[candidate %zu] score=%d arity=%d %s", i, candidates[i].score,
               candidates[i].arity, candidates[i].name.c_str());
    }

    size_t hookCount = (std::min)(candidates.size(), kMaxHooks);
    size_t installed = 0;

    for (size_t i = 0; i < hookCount; i++) {
        const SymbolCandidate& c = candidates[i];
        BOOL ok;
        if (c.arity == 0) {
            ok = Wh_SetFunctionHook(c.address, g_hookTable.hook0[installed],
                                    g_hookTable.orig0[installed]);
        } else {
            ok = Wh_SetFunctionHook(c.address, g_hookTable.hook2[installed],
                                    g_hookTable.orig2[installed]);
        }
        if (ok) {
            Wh_Log(L"Hooked (arity %d): %s", c.arity, c.name.c_str());
            installed++;
        } else {
            Wh_Log(L"Hook failed: %s", c.name.c_str());
        }
    }

    if (installed == 0) {
        Wh_Log(L"No hooks installed");
        return false;
    }
    Wh_Log(L"%zu hook(s) installed - waiting for the taskbar to call one", installed);
    return true;
}

// ---------------------------------------------------------------------------
// Windhawk lifecycle
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    Wh_Log(L"Init");
    LoadSettings();

    if (!HookTaskbarView()) {
        // Stay loaded and inert rather than unloading, so the diagnostic log
        // above survives and settings can be changed without a reinstall.
        Wh_Log(L"Mod loaded but inactive - no TaskbarFrame hook.");
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L"AfterInit");

    g_eventThread = CreateThread(nullptr, 0, EventThreadProc, nullptr, 0, &g_eventThreadId);
    if (!g_eventThread) {
        Wh_Log(L"Failed to create event thread: %u", GetLastError());
        return;
    }

    // Lay out once for whatever is focused right now.
    ScheduleLayout(false);
}

void Wh_ModBeforeUninit() {
    Wh_Log(L"BeforeUninit");

    // Stop new foreground events first, but keep dispatched work runnable so
    // the restore below can still execute.
    g_stopEvents.store(true);

    if (g_eventThread) {
        PostThreadMessageW(g_eventThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_eventThread, 5000);
        CloseHandle(g_eventThread);
        g_eventThread = nullptr;
    }

    // Put the taskbar back the way we found it, with no animation so it
    // completes immediately.
    g_settings.animationMs = 0;
    g_settings.settleDelayMs = 0;
    ScheduleLayout(true);

    // Now block anything new and wait out whatever is still in flight, so no
    // queued callback runs after this DLL is unmapped.
    g_unloading.store(true);
    for (int i = 0; i < 250 && g_pendingCallbacks.load() > 0; i++) {
        Sleep(20);
    }
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
    {
        std::lock_guard<std::mutex> lock(g_framesMutex);
        g_frames.clear();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
        g_frameStates.clear();
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");
    LoadSettings();
    ScheduleLayout(false);
}