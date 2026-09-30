// ==WindhawkMod==
// @id              wt-test-harness
// @name            Windows Terminal test harness (development only)
// @description     Keeps a test Windows Terminal off the user's desktop: windows open on a named virtual desktop and never take focus
// @version         1.0.0
// @author          Lattice Labs
// @github          https://github.com/addievo
// @include         WindowsTerminal.exe
// @architecture    x86-64
// @compilerOptions -lole32 -luuid -ladvapi32
// @license         MIT
// ==/WindhawkMod==

// Development tool for wt-vertical-tabs, not part of the mod. Install it with
// tools/build.py --source tools/test-harness.wh.cpp --include <path of a
// portable test WindowsTerminal.exe>, never for the Windows Terminal you use.
//
// Test runs drive a terminal for minutes at a time. Windows Terminal brings
// its window to the foreground when it starts and when it receives a command
// line, and once the user has been idle past the foreground lock timeout
// Windows lets it, so keystrokes meant for something else land in the test
// terminal, or Windows switches to whichever virtual desktop it is on. This
// harness makes the test terminal unable to activate at all, and creates its
// windows directly on a virtual desktop named "WT test" (made beforehand with
// Win+Tab, or any virtual desktop tool), so nothing appears on the user's
// screen. UI Automation can still drive it and PrintWindow can still capture
// it there.

#include <shobjidl.h>
#include <windhawk_utils.h>

#include <string>

constexpr wchar_t kTestDesktopName[] = L"WT test";

bool FindTestDesktop(GUID* desktopId) {
    HKEY desktops;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\"
                      L"VirtualDesktops\\Desktops",
                      0, KEY_READ, &desktops) != ERROR_SUCCESS) {
        return false;
    }

    bool found = false;
    WCHAR subkey[64];
    for (DWORD i = 0;; i++) {
        DWORD length = ARRAYSIZE(subkey);
        if (RegEnumKeyExW(desktops, i, subkey, &length, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS) {
            break;
        }
        WCHAR name[128];
        DWORD size = sizeof(name);
        if (RegGetValueW(desktops, subkey, L"Name", RRF_RT_REG_SZ, nullptr, name,
                         &size) == ERROR_SUCCESS &&
            wcscmp(name, kTestDesktopName) == 0 &&
            SUCCEEDED(CLSIDFromString(subkey, desktopId))) {
            found = true;
            break;
        }
    }
    RegCloseKey(desktops);
    return found;
}

void MoveToTestDesktop(HWND window) {
    // WTVT_TEST_DESKTOP=none leaves the window on the current desktop, for
    // runs where a person tests by hand (the window still never activates).
    WCHAR target[16];
    if (GetEnvironmentVariableW(L"WTVT_TEST_DESKTOP", target, ARRAYSIZE(target)) &&
        _wcsicmp(target, L"none") == 0) {
        return;
    }
    GUID desktopId;
    if (!FindTestDesktop(&desktopId)) {
        Wh_Log(L"No virtual desktop named \"%s\"", kTestDesktopName);
        return;
    }
    IVirtualDesktopManager* manager = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&manager));
    if (SUCCEEDED(hr)) {
        hr = manager->MoveWindowToDesktop(window, desktopId);
        manager->Release();
    }
    Wh_Log(L"MoveWindowToDesktop(%p): %08X", window, hr);
}

bool IsTerminalWindow(HWND window) {
    WCHAR className[64];
    return GetClassNameW(window, className, ARRAYSIZE(className)) &&
           wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0;
}

////////////////////////////////////////////////////////////////////////////////
// Tracing, off unless WTVT_TEST_TRACE=1 is set in the test terminal's
// environment: what a real mouse click delivers, and where menu popups come
// from, for working out how a test can reproduce an interaction.

bool g_trace = false;

void LogStack(PCWSTR what) {
    void* frames[40];
    USHORT count = CaptureStackBackTrace(1, ARRAYSIZE(frames), frames, nullptr);
    Wh_Log(L"stack for %s (%u frames):", what, count);
    for (USHORT i = 0; i < count; i++) {
        HMODULE module = nullptr;
        WCHAR name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)frames[i], &module)) {
            GetModuleFileNameW(module, name, ARRAYSIZE(name));
        }
        PCWSTR base = wcsrchr(name, L'\\');
        Wh_Log(L"  #%02u %s+0x%llX", i, base ? base + 1 : name,
               (unsigned long long)((BYTE*)frames[i] - (BYTE*)module));
    }
}

PCWSTR MessageName(UINT message) {
    switch (message) {
        case WM_LBUTTONDOWN: return L"WM_LBUTTONDOWN";
        case WM_LBUTTONUP: return L"WM_LBUTTONUP";
        case WM_RBUTTONDOWN: return L"WM_RBUTTONDOWN";
        case WM_RBUTTONUP: return L"WM_RBUTTONUP";
        case WM_NCRBUTTONDOWN: return L"WM_NCRBUTTONDOWN";
        case WM_NCRBUTTONUP: return L"WM_NCRBUTTONUP";
        case WM_CONTEXTMENU: return L"WM_CONTEXTMENU";
        case WM_POINTERDOWN: return L"WM_POINTERDOWN";
        case WM_POINTERUP: return L"WM_POINTERUP";
        case WM_POINTERACTIVATE: return L"WM_POINTERACTIVATE";
        case WM_MOUSEACTIVATE: return L"WM_MOUSEACTIVATE";
        default: return nullptr;
    }
}

using DispatchMessageW_t = decltype(&DispatchMessageW);
DispatchMessageW_t DispatchMessageW_Original;
LRESULT WINAPI DispatchMessageW_Hook(const MSG* msg) {
    if (PCWSTR name = msg ? MessageName(msg->message) : nullptr) {
        WCHAR className[128] = L"";
        GetClassNameW(msg->hwnd, className, ARRAYSIZE(className));
        Wh_Log(L"input %s hwnd=%p (%s) wParam=%llX lParam=%llX pos=(%d,%d) extra=%llX",
               name, msg->hwnd, className, (unsigned long long)msg->wParam,
               (unsigned long long)msg->lParam, (short)LOWORD(msg->lParam),
               (short)HIWORD(msg->lParam), (unsigned long long)GetMessageExtraInfo());
        if (msg->message >= WM_POINTERDOWN && msg->message <= WM_POINTERUP) {
            POINTER_INFO info{};
            if (GetPointerInfo(GET_POINTERID_WPARAM(msg->wParam), &info)) {
                Wh_Log(L"  pointer type=%u flags=%X buttonChange=%u pixel=(%ld,%ld)",
                       info.pointerType, info.pointerFlags, info.ButtonChangeType,
                       info.ptPixelLocation.x, info.ptPixelLocation.y);
            }
        }
    }
    return DispatchMessageW_Original(msg);
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;
HWND WINAPI CreateWindowExW_Hook(DWORD exStyle, LPCWSTR className, LPCWSTR windowName,
                                 DWORD style, int x, int y, int width, int height,
                                 HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    if (g_trace && !IS_INTRESOURCE(className) &&
        wcscmp(className, L"Xaml_WindowedPopupClass") == 0) {
        LogStack(L"popup window creation");
    }
    bool terminal = !IS_INTRESOURCE(className) &&
                    wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0;
    if (terminal) {
        exStyle |= WS_EX_NOACTIVATE;
        style &= ~WS_VISIBLE;
    }
    HWND window = CreateWindowExW_Original(exStyle, className, windowName, style, x, y,
                                           width, height, parent, menu, instance, param);
    if (window && terminal) {
        MoveToTestDesktop(window);
    }
    return window;
}

using ShowWindow_t = decltype(&ShowWindow);
ShowWindow_t ShowWindow_Original;
BOOL WINAPI ShowWindow_Hook(HWND window, int command) {
    WCHAR className[64];
    if (g_trace && command != SW_HIDE &&
        GetClassNameW(window, className, ARRAYSIZE(className)) &&
        wcscmp(className, L"Xaml_WindowedPopupClass") == 0) {
        LogStack(L"popup window show");
    }
    switch (command) {
        case SW_SHOWNORMAL:
        case SW_RESTORE:
        case SW_SHOWDEFAULT:
            command = SW_SHOWNOACTIVATE;
            break;
        case SW_SHOW:
        case SW_SHOWMAXIMIZED:
            command = SW_SHOWNA;
            break;
        case SW_SHOWMINIMIZED:
            command = SW_SHOWMINNOACTIVE;
            break;
    }
    return ShowWindow_Original(window, command);
}

using SetWindowPos_t = decltype(&SetWindowPos);
SetWindowPos_t SetWindowPos_Original;
BOOL WINAPI SetWindowPos_Hook(HWND window, HWND insertAfter, int x, int y, int cx, int cy,
                              UINT flags) {
    if (!GetParent(window)) {
        flags |= SWP_NOACTIVATE;
        if (insertAfter == HWND_TOP || insertAfter == HWND_TOPMOST) {
            flags |= SWP_NOZORDER;
        }
    }
    return SetWindowPos_Original(window, insertAfter, x, y, cx, cy, flags);
}

using SetForegroundWindow_t = decltype(&SetForegroundWindow);
SetForegroundWindow_t SetForegroundWindow_Original;
BOOL WINAPI SetForegroundWindow_Hook(HWND) {
    return TRUE;
}

using BringWindowToTop_t = decltype(&BringWindowToTop);
BringWindowToTop_t BringWindowToTop_Original;
BOOL WINAPI BringWindowToTop_Hook(HWND) {
    return TRUE;
}

using SwitchToThisWindow_t = void(WINAPI*)(HWND, BOOL);
SwitchToThisWindow_t SwitchToThisWindow_Original;
void WINAPI SwitchToThisWindow_Hook(HWND, BOOL) {}

// When a wt.exe command line reaches an existing window, Terminal "summons" it:
// it attaches its input to the foreground thread's and calls SetActiveWindow,
// which makes it the foreground window (and switches virtual desktops) past the
// foreground lock. Neither may happen in a test Terminal.
using AttachThreadInput_t = decltype(&AttachThreadInput);
AttachThreadInput_t AttachThreadInput_Original;
BOOL WINAPI AttachThreadInput_Hook(DWORD attach, DWORD attachTo, BOOL doAttach) {
    if (doAttach) {
        return TRUE;
    }
    return AttachThreadInput_Original(attach, attachTo, doAttach);
}

using SetActiveWindow_t = decltype(&SetActiveWindow);
SetActiveWindow_t SetActiveWindow_Original;
HWND WINAPI SetActiveWindow_Hook(HWND window) {
    if (window && !(GetWindowLongW(window, GWL_STYLE) & WS_CHILD)) {
        return GetActiveWindow();
    }
    return SetActiveWindow_Original(window);
}

BOOL Wh_ModInit() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    Wh_SetFunctionHook((void*)CreateWindowExW, (void*)CreateWindowExW_Hook,
                       (void**)&CreateWindowExW_Original);
    WCHAR trace[8] = L"";
    DWORD length = GetEnvironmentVariableW(L"WTVT_TEST_TRACE", trace, ARRAYSIZE(trace));
    g_trace = length > 0 && length < ARRAYSIZE(trace) && wcscmp(trace, L"1") == 0;
    if (g_trace) {
        Wh_SetFunctionHook((void*)DispatchMessageW, (void*)DispatchMessageW_Hook,
                           (void**)&DispatchMessageW_Original);
    }
    Wh_SetFunctionHook((void*)ShowWindow, (void*)ShowWindow_Hook,
                       (void**)&ShowWindow_Original);
    Wh_SetFunctionHook((void*)SetWindowPos, (void*)SetWindowPos_Hook,
                       (void**)&SetWindowPos_Original);
    Wh_SetFunctionHook((void*)SetForegroundWindow, (void*)SetForegroundWindow_Hook,
                       (void**)&SetForegroundWindow_Original);
    Wh_SetFunctionHook((void*)BringWindowToTop, (void*)BringWindowToTop_Hook,
                       (void**)&BringWindowToTop_Original);
    Wh_SetFunctionHook((void*)AttachThreadInput, (void*)AttachThreadInput_Hook,
                       (void**)&AttachThreadInput_Original);
    Wh_SetFunctionHook((void*)SetActiveWindow, (void*)SetActiveWindow_Hook,
                       (void**)&SetActiveWindow_Original);
    if (auto switchToThisWindow = GetProcAddress(user32, "SwitchToThisWindow")) {
        Wh_SetFunctionHook((void*)switchToThisWindow, (void*)SwitchToThisWindow_Hook,
                           (void**)&SwitchToThisWindow_Original);
    }
    return TRUE;
}
