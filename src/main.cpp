// VirtTilt - switch Windows 11 virtual desktops by tilting the mouse wheel.
//
// Design notes:
//  * A low-level mouse hook (WH_MOUSE_LL) watches for WM_MOUSEHWHEEL (wheel tilt).
//    The hook callback must return fast or Windows silently drops the hook, so it
//    only posts a message; the actual switching happens in the message loop.
//  * Switching is done by injecting Ctrl+Win+Left/Right. This is stable across
//    Windows builds, unlike the undocumented IVirtualDesktopManagerInternal COM API.
//  * Settings live in HKCU\Software\VirtTilt, autostart in HKCU\...\Run.

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <servprov.h>
#include <shobjidl.h>
#include <dwmapi.h>

#include <stdarg.h>
#include <wchar.h>

namespace {

constexpr wchar_t kAppName[]     = L"VirtTilt";
constexpr wchar_t kWindowClass[] = L"VirtTilt.HiddenWindow";
constexpr wchar_t kMutexName[]   = L"Local\\VirtTilt.SingleInstance";
constexpr wchar_t kSettingsKey[] = L"Software\\VirtTilt";
constexpr wchar_t kRunKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;
constexpr UINT WM_DESKTOP_SWITCH = WM_APP + 2;
constexpr UINT WM_LOG_MOUSE = WM_APP + 3;  // diagnostics only (--log)
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kTrayRetryTimer = 1;
constexpr int kTrayRetryMax = 60;  // Explorer may not be ready right after login.

enum MenuId : UINT {
    ID_ENABLED = 100,
    ID_INVERT,
    ID_SWALLOW,
    ID_ANIMATE,
    ID_MODE_API,
    ID_MODE_KEYS,
    ID_SPEED_FAST,     // 150 ms
    ID_SPEED_NORMAL,   // 180 ms
    ID_SPEED_RELAXED,  // 220 ms
    ID_SPEED_SAFE,     // 260 ms
    ID_AUTOSTART,
    ID_EXIT,
};

struct Settings {
    bool enabled = true;
    bool invert = false;       // false: tilt right = next desktop
    bool swallow = true;       // block the horizontal scroll from reaching apps
    bool useApi = true;        // true: COM API (bypasses RDP), false: Ctrl+Win+Arrow
    bool animate = true;       // true: smooth slide animation in API mode
    DWORD gestureGapMs = 180;  // debounce window: blocks hardware tilt release (~105ms) while allowing rapid double-clicks (~220ms)
};

Settings g_settings;
HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
HHOOK g_hook = nullptr;
HICON g_iconActive = nullptr;
HICON g_iconPaused = nullptr;
UINT g_taskbarCreatedMsg = 0;
bool g_trayAdded = false;
int g_trayRetries = 0;
bool g_logEnabled = false;  // set by the --log command line switch

// ---------------------------------------------------------------------------
// Diagnostics: run with --log to append to %TEMP%\VirtTilt.log
// ---------------------------------------------------------------------------

void Log(const char* format, ...) {
    if (!g_logEnabled) return;
    char line[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int len = wsprintfA(line, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond,
                        st.wMilliseconds);
    va_list args;
    va_start(args, format);
    len += wvsprintfA(line + len, format, args);
    va_end(args);
    line[len++] = '\r';
    line[len++] = '\n';

    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH - 16, path);
    if (n == 0) return;
    lstrcatW(path, L"VirtTilt.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, line, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(file);
}

void LogEnvironment() {
    if (!g_logEnabled) return;
    char station[128] = "?", desktop[128] = "?";
    DWORD needed;
    GetUserObjectInformationA(GetProcessWindowStation(), UOI_NAME, station, sizeof(station),
                              &needed);
    GetUserObjectInformationA(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop,
                              sizeof(desktop), &needed);
    char inputDesktop[128] = "?";
    if (HDESK input = OpenInputDesktop(0, FALSE, GENERIC_READ)) {
        GetUserObjectInformationA(input, UOI_NAME, inputDesktop, sizeof(inputDesktop), &needed);
        CloseDesktop(input);
    }
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Log("start: session=%lu winsta=%s desktop=%s inputDesktop=%s tray=%p", session, station,
        desktop, inputDesktop, FindWindowW(L"Shell_TrayWnd", nullptr));
}

// ---------------------------------------------------------------------------
// Registry helpers
// ---------------------------------------------------------------------------

DWORD ReadDword(const wchar_t* name, DWORD fallback) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr,
                     &value, &size) == ERROR_SUCCESS) {
        return value;
    }
    return fallback;
}

void WriteDword(const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                       sizeof(value));
        RegCloseKey(key);
    }
}

void LoadSettings() {
    g_settings.enabled = ReadDword(L"Enabled", 1) != 0;
    g_settings.invert = ReadDword(L"Invert", 0) != 0;
    g_settings.swallow = ReadDword(L"BlockHorizontalScroll", 1) != 0;
    g_settings.useApi = ReadDword(L"UseApi", 1) != 0;
    g_settings.animate = ReadDword(L"Animate", 1) != 0;
    DWORD gap = ReadDword(L"GestureGapMs", 180);
    if (gap <= 130 || gap == 250) {
        gap = 180; // Migrate old defaults (0, 70, 100, 250ms) to 180ms
        WriteDword(L"GestureGapMs", 180);
    }
    g_settings.gestureGapMs = gap;
}

void SaveSettings() {
    WriteDword(L"Enabled", g_settings.enabled);
    WriteDword(L"Invert", g_settings.invert);
    WriteDword(L"BlockHorizontalScroll", g_settings.swallow);
    WriteDword(L"UseApi", g_settings.useApi ? 1 : 0);
    WriteDword(L"Animate", g_settings.animate ? 1 : 0);
    WriteDword(L"GestureGapMs", g_settings.gestureGapMs);
}

bool IsAutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr,
                        nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool enable) {
    if (!enable) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName);
        return;
    }
    wchar_t path[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) return;

    wchar_t command[ARRAYSIZE(path) + 3];
    wsprintfW(command, L"\"%s\"", path);

    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command),
                       (lstrlenW(command) + 1) * sizeof(wchar_t));
        RegCloseKey(key);
    }
}

// ---------------------------------------------------------------------------
// COM-based Virtual Desktop switching (undocumented internal shell API)
// ---------------------------------------------------------------------------

static const CLSID CLSID_ImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
static const CLSID CLSID_VirtualDesktopManagerInternal = {
    0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};

// Candidate IIDs for IVirtualDesktopManagerInternal across Windows versions
static const IID kCandidateIIDs[] = {
    // Windows 11 22621 / 22631 / 26100+ (22H2, 23H2, 24H2)
    {0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}},
    // Windows 11 22000 (21H2)
    {0xB2F925B9, 0x5A0F, 0x4D2E, {0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10}},
    // Windows 10 Build 20348
    {0x094AFE11, 0x44F2, 0x4BA0, {0x97, 0x6F, 0x29, 0xA9, 0x7E, 0x26, 0x3E, 0xE0}},
    // Windows 10 Build < 20348
    {0xF31574D6, 0xB682, 0x4CDC, {0xBD, 0x56, 0x18, 0x27, 0x86, 0x0A, 0xBE, 0xC6}},
};

class VirtualDesktopApiManager {
public:
    VirtualDesktopApiManager() = default;
    ~VirtualDesktopApiManager() { Release(); }

    void Release() {
        if (m_pInternal) {
            reinterpret_cast<IUnknown*>(m_pInternal)->Release();
            m_pInternal = nullptr;
        }
        if (m_pShell) {
            m_pShell->Release();
            m_pShell = nullptr;
        }
        if (m_pVdm) {
            m_pVdm->Release();
            m_pVdm = nullptr;
        }
        m_initialized = false;
    }

    bool EnsureInitialized() {
        if (m_initialized && m_pInternal) return true;
        Release();

        HRESULT hr = CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
                                      IID_IServiceProvider, reinterpret_cast<void**>(&m_pShell));
        if (FAILED(hr) || !m_pShell) {
            Log("VirtualDesktop API: CoCreateInstance(ImmersiveShell) failed: 0x%08lX", hr);
            return false;
        }

        if (!m_buildNumber) {
            OSVERSIONINFOEXW osv = {};
            osv.dwOSVersionInfoSize = sizeof(osv);
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) {
                using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
                auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
                if (fn && fn(&osv) == 0) {
                    m_buildNumber = osv.dwBuildNumber;
                }
            }
        }

        for (const auto& iid : kCandidateIIDs) {
            hr = m_pShell->QueryService(CLSID_VirtualDesktopManagerInternal, iid,
                                        reinterpret_cast<void**>(&m_pInternal));
            if (SUCCEEDED(hr) && m_pInternal) {
                m_matchedIid = iid;
                m_initialized = true;
                Log("VirtualDesktop API: connected with IID {%08lX-...} (Windows build %lu)", iid.Data1, m_buildNumber);
                break;
            }
        }

        if (!m_pInternal) {
            Log("VirtualDesktop API: failed to query IVirtualDesktopManagerInternal");
            Release();
            return false;
        }

        if (!m_pVdm) {
            CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                             IID_IVirtualDesktopManager, reinterpret_cast<void**>(&m_pVdm));
        }

        return true;
    }

    IVirtualDesktopManager* GetVdm() const { return m_pVdm; }

    int GetAnimationVTableIndex() const {
        if (m_buildNumber >= 26100) return 22; // Windows 11 24H2+ (e.g. 26100, 26300)
        if (m_buildNumber >= 22621) return 21; // Windows 11 22H2 / 23H2
        return -1;
    }

    bool SwitchDesktop(bool next, bool animate, bool* didSwitch = nullptr) {
        if (didSwitch) *didSwitch = false;
        if (!EnsureInitialized()) return false;

        void** vtbl = *reinterpret_cast<void***>(m_pInternal);
        // Index 6: GetCurrentDesktop(IUnknown** ppDesktop)
        using PFN_GetCurrentDesktop = HRESULT(STDMETHODCALLTYPE*)(void*, void**);
        // Index 8: GetAdjacentDesktop(IUnknown* pFrom, int direction, IUnknown** ppDesktop)
        using PFN_GetAdjacentDesktop = HRESULT(STDMETHODCALLTYPE*)(void*, void*, int, void**);
        // Index 9: SwitchDesktop(IUnknown* pDesktop)
        using PFN_SwitchDesktop = HRESULT(STDMETHODCALLTYPE*)(void*, void*);
        // Animated switch: SwitchDesktopWithAnimation(IUnknown* pDesktop)
        using PFN_SwitchDesktopWithAnimation = HRESULT(STDMETHODCALLTYPE*)(void*, void*);

        auto fnGetCurrent = reinterpret_cast<PFN_GetCurrentDesktop>(vtbl[6]);
        auto fnGetAdjacent = reinterpret_cast<PFN_GetAdjacentDesktop>(vtbl[8]);
        auto fnSwitch = reinterpret_cast<PFN_SwitchDesktop>(vtbl[9]);

        void* pCurrent = nullptr;
        HRESULT hr = fnGetCurrent(m_pInternal, &pCurrent);
        if (FAILED(hr) || !pCurrent) {
            Log("VirtualDesktop API: GetCurrentDesktop failed 0x%08lX, retrying init", hr);
            Release();
            if (!EnsureInitialized()) return false;
            vtbl = *reinterpret_cast<void***>(m_pInternal);
            fnGetCurrent = reinterpret_cast<PFN_GetCurrentDesktop>(vtbl[6]);
            fnGetAdjacent = reinterpret_cast<PFN_GetAdjacentDesktop>(vtbl[8]);
            fnSwitch = reinterpret_cast<PFN_SwitchDesktop>(vtbl[9]);
            hr = fnGetCurrent(m_pInternal, &pCurrent);
            if (FAILED(hr) || !pCurrent) return false;
        }

        // Direction: 3 = Left, 4 = Right
        const int direction = next ? 4 : 3;
        void* pAdjacent = nullptr;
        hr = fnGetAdjacent(m_pInternal, pCurrent, direction, &pAdjacent);
        reinterpret_cast<IUnknown*>(pCurrent)->Release();

        if (FAILED(hr) || !pAdjacent) {
            Log("VirtualDesktop API: no adjacent desktop in direction %d (hr=0x%08lX)", direction, hr);
            return true; // Already at the edge; succeeded in attempting
        }

        if (animate) {
            const int animIndex = GetAnimationVTableIndex();
            if (animIndex > 0) {
                auto fnSwitchAnim = reinterpret_cast<PFN_SwitchDesktopWithAnimation>(vtbl[animIndex]);
                hr = fnSwitchAnim(m_pInternal, pAdjacent);
                reinterpret_cast<IUnknown*>(pAdjacent)->Release();
                Log("VirtualDesktop API: SwitchDesktopWithAnimation result 0x%08lX", hr);
                if (SUCCEEDED(hr)) {
                    if (didSwitch) *didSwitch = true;
                    return true;
                }
                return false;
            }
        }

        hr = fnSwitch(m_pInternal, pAdjacent);
        reinterpret_cast<IUnknown*>(pAdjacent)->Release();
        Log("VirtualDesktop API: SwitchDesktop result 0x%08lX", hr);
        if (SUCCEEDED(hr)) {
            if (didSwitch) *didSwitch = true;
            return true;
        }
        return false;
    }

private:
    IServiceProvider* m_pShell = nullptr;
    void* m_pInternal = nullptr;
    IVirtualDesktopManager* m_pVdm = nullptr;
    IID m_matchedIid = {};
    DWORD m_buildNumber = 0;
    bool m_initialized = false;
};

VirtualDesktopApiManager g_vdApi;

// ---------------------------------------------------------------------------
// Desktop switching (Keystroke method)
// ---------------------------------------------------------------------------

void SetKey(INPUT& in, WORD vk, bool up) {
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0);
    if (vk != VK_LCONTROL) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;  // Win and arrows are extended
}

// Remote desktop and VM clients install a low-level keyboard hook while focused
// and forward Windows-key combinations to the remote machine, which swallows our
// injected Ctrl+Win+Arrow.
bool IsKeyboardCapturingWindow(HWND hwnd) {
    if (!hwnd) return false;

    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (lstrcmpiW(cls, L"TscShellContainerClass") == 0) return true;  // mstsc / msrdc

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD size = ARRAYSIZE(path);
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok) return false;

    const wchar_t* exe = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\') exe = p + 1;
    }
    static const wchar_t* const kCapturing[] = {
        L"mstsc.exe", L"msrdc.exe", L"vmconnect.exe", L"vmware.exe", L"vmware-vmx.exe",
        L"VirtualBoxVM.exe",
    };
    for (const wchar_t* name : kCapturing) {
        if (lstrcmpiW(exe, name) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Background worker thread queue for desktop switching
//
// Desktop switching (especially COM SwitchDesktopWithAnimation) blocks for
// ~180-230ms while the shell animates. If executed on the UI thread, it
// stalls the low-level mouse hook (WH_MOUSE_LL), queuing up hardware bounce
// packets which then appear to arrive 200ms late and trigger duplicate switches.
// Moving the switch execution to a background worker keeps the hook callback
// under 5 microseconds, preserving accurate real-time debounce timing.
// ---------------------------------------------------------------------------

constexpr int kMaxQueue = 8;
int g_queue[kMaxQueue] = {};
int g_queueHead = 0;
int g_queueTail = 0;
CRITICAL_SECTION g_queueCs;
HANDLE g_switchEvent = nullptr;
HANDLE g_workerThread = nullptr;
bool g_workerExit = false;

void EnqueueSwitch(int dir) {
    EnterCriticalSection(&g_queueCs);
    int nextTail = (g_queueTail + 1) % kMaxQueue;
    if (nextTail != g_queueHead) {
        g_queue[g_queueTail] = dir;
        g_queueTail = nextTail;
        SetEvent(g_switchEvent);
    }
    LeaveCriticalSection(&g_queueCs);
}

bool DequeueSwitch(int& dir) {
    EnterCriticalSection(&g_queueCs);
    bool found = false;
    if (g_queueHead != g_queueTail) {
        dir = g_queue[g_queueHead];
        g_queueHead = (g_queueHead + 1) % kMaxQueue;
        found = true;
    }
    LeaveCriticalSection(&g_queueCs);
    return found;
}

// ---------------------------------------------------------------------------
// Window focus management and enumeration
// ---------------------------------------------------------------------------

void GetWindowDetails(HWND hwnd, char* outClass, int maxClass, char* outTitle, int maxTitle, char* outExe, int maxExe) {
    if (maxClass > 0) outClass[0] = '\0';
    if (maxTitle > 0) outTitle[0] = '\0';
    if (maxExe > 0) outExe[0] = '\0';
    if (!hwnd || !IsWindow(hwnd)) return;

    if (maxClass > 0) GetClassNameA(hwnd, outClass, maxClass);
    if (maxTitle > 0) GetWindowTextA(hwnd, outTitle, maxTitle);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            const wchar_t* exe = path;
            for (const wchar_t* p = path; *p; ++p) {
                if (*p == L'\\') exe = p + 1;
            }
            WideCharToMultiByte(CP_ACP, 0, exe, -1, outExe, maxExe, nullptr, nullptr);
        }
        CloseHandle(process);
    }
}

bool ForceForegroundWindow(HWND target) {
    if (!target || !IsWindow(target)) return false;

    HWND foreground = GetForegroundWindow();
    if (foreground == target) return true;

    const DWORD currentThread = GetCurrentThreadId();
    const DWORD fgThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD targetThread = GetWindowThreadProcessId(target, nullptr);

    bool attachedFg = false;
    bool attachedTarget = false;

    if (fgThread && fgThread != currentThread) {
        attachedFg = (AttachThreadInput(currentThread, fgThread, TRUE) != FALSE);
    }
    if (targetThread && targetThread != currentThread && targetThread != fgThread) {
        attachedTarget = (AttachThreadInput(currentThread, targetThread, TRUE) != FALSE);
    }

    AllowSetForegroundWindow(ASFW_ANY);
    BringWindowToTop(target);
    SetForegroundWindow(target);

    // If still not foreground, simulate a neutral Alt keystroke to unlock permissions
    if (GetForegroundWindow() != target) {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_MENU;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_MENU;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
        SetForegroundWindow(target);
    }

    if (attachedTarget) AttachThreadInput(currentThread, targetThread, FALSE);
    if (attachedFg) AttachThreadInput(currentThread, fgThread, FALSE);

    for (int i = 0; i < 10 && GetForegroundWindow() != target; ++i) {
        Sleep(5);
    }

    return GetForegroundWindow() == target;
}

// Moves keyboard focus to the taskbar (it lives on every virtual desktop), so
// the capturing client's hook stops forwarding keys.
void MoveFocusToTaskbar(HWND foreground) {
    (void)foreground;
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return;
    const bool ok = ForceForegroundWindow(taskbar);
    Log("moved focus to taskbar: %s", ok ? "ok" : "FAILED");
}

struct FindTopContext {
    IVirtualDesktopManager* vdm = nullptr;
    HWND found = nullptr;
};

bool IsEligibleTopWindow(HWND hwnd, IVirtualDesktopManager* vdm) {
    if (!hwnd || hwnd == g_hwnd) return false;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;

    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (lstrcmpiW(cls, L"Shell_TrayWnd") == 0 ||
        lstrcmpiW(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        lstrcmpiW(cls, L"Progman") == 0 ||
        lstrcmpiW(cls, L"WorkerW") == 0 ||
        lstrcmpiW(cls, L"Windows.UI.Core.CoreWindow") == 0) {
        return false;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW)) {
        return false;
    }

    RECT rc;
    if (!GetWindowRect(hwnd, &rc) || (rc.right - rc.left <= 0) || (rc.bottom - rc.top <= 0)) {
        return false;
    }

    // Windows on inactive virtual desktops are cloaked by DWM.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }

    if (vdm) {
        BOOL onCurrent = FALSE;
        if (SUCCEEDED(vdm->IsWindowOnCurrentVirtualDesktop(hwnd, &onCurrent)) && !onCurrent) {
            return false;
        }
    }

    return true;
}

HWND FindTopWindowOnCurrentDesktop(IVirtualDesktopManager* vdm) {
    if (g_queueHead != g_queueTail) return nullptr;

    FindTopContext ctx = { vdm, nullptr };
    for (int attempt = 0; attempt < 8; ++attempt) {
        ctx.found = nullptr;
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* pCtx = reinterpret_cast<FindTopContext*>(lParam);
            if (IsEligibleTopWindow(hwnd, pCtx->vdm)) {
                pCtx->found = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.found) {
            HWND popup = GetLastActivePopup(ctx.found);
            if (popup && IsWindow(popup) && IsWindowVisible(popup) && !IsIconic(popup)) {
                DWORD cloaked = 0;
                if (SUCCEEDED(DwmGetWindowAttribute(popup, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked == 0) {
                    ctx.found = popup;
                }
            }
            return ctx.found;
        }

        if (g_queueHead != g_queueTail) return nullptr;
        Sleep(15);
    }
    return nullptr;
}

void FocusTopWindowOnCurrentDesktop() {
    HWND target = FindTopWindowOnCurrentDesktop(g_vdApi.GetVdm());
    if (!target) {
        // Fallback: if no eligible top-level app window on new desktop,
        // move focus to the taskbar to release any capturing/previous window.
        target = FindWindowW(L"Shell_TrayWnd", nullptr);
    }

    if (target) {
        if (g_logEnabled) {
            char cls[64] = {}, title[128] = {}, exe[64] = {};
            GetWindowDetails(target, cls, sizeof(cls), title, sizeof(title), exe, sizeof(exe));
            Log("focusing window on new desktop: hwnd=%p exe=%s class=%s title='%s'",
                target, exe, cls, title);
        }

        const bool ok = ForceForegroundWindow(target);
        if (g_logEnabled) {
            Log("focus transfer result: %s (active fg=%p)", ok ? "ok" : "FAILED", GetForegroundWindow());
        }
    }
}

void SwitchDesktopKeys(bool next) {
    HWND foreground = GetForegroundWindow();
    const bool capturing = IsKeyboardCapturingWindow(foreground);
    if (g_logEnabled) {
        char cls[64] = {};
        GetClassNameA(foreground, cls, sizeof(cls));
        Log("foreground window class=%s capturing=%d", cls, capturing);
    }
    if (capturing) {
        MoveFocusToTaskbar(foreground);
    }

    const WORD arrow = next ? VK_RIGHT : VK_LEFT;
    INPUT inputs[6] = {};
    SetKey(inputs[0], VK_LCONTROL, false);
    SetKey(inputs[1], VK_LWIN, false);
    SetKey(inputs[2], arrow, false);
    SetKey(inputs[3], arrow, true);
    SetKey(inputs[4], VK_LWIN, true);
    SetKey(inputs[5], VK_LCONTROL, true);
    SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
}

void SwitchDesktop(bool next) {
    HWND fgBefore = GetForegroundWindow();
    if (g_logEnabled) {
        char cls[64] = {}, title[128] = {}, exe[64] = {};
        GetWindowDetails(fgBefore, cls, sizeof(cls), title, sizeof(title), exe, sizeof(exe));
        Log("before switch: fg=%p exe=%s class=%s title='%s'", fgBefore, exe, cls, title);
    }

    if (g_settings.useApi) {
        bool didSwitch = false;
        if (g_vdApi.SwitchDesktop(next, g_settings.animate, &didSwitch)) {
            Log("switched desktop via API -> %s (animated=%d, didSwitch=%d)",
                next ? "next" : "previous", g_settings.animate, didSwitch);
            if (didSwitch) {
                FocusTopWindowOnCurrentDesktop();
            }
            return;
        }
        Log("switch desktop via API failed, falling back to keystroke");
    }
    SwitchDesktopKeys(next);
}

DWORD WINAPI SwitchWorkerThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    while (true) {
        WaitForSingleObject(g_switchEvent, INFINITE);
        if (g_workerExit) break;

        int dir = 0;
        while (DequeueSwitch(dir)) {
            if (g_workerExit) break;
            bool next = dir > 0;
            if (g_settings.invert) next = !next;
            SwitchDesktop(next);
        }
    }

    g_vdApi.Release();
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// Mouse hook
// ---------------------------------------------------------------------------

LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (g_logEnabled && code == HC_ACTION && wParam != WM_MOUSEMOVE) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        PostMessageW(g_hwnd, WM_LOG_MOUSE, wParam,
                     static_cast<LPARAM>(info->mouseData) | (static_cast<LPARAM>(info->flags) << 32));
    }
    if (code == HC_ACTION && wParam == WM_MOUSEHWHEEL) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const short delta = static_cast<short>(HIWORD(info->mouseData));
        if (delta != 0) {
            static DWORD lastTriggerTime = 0;
            static int lastDir = 0;
            static bool hasLast = false;

            const int dir = delta > 0 ? 1 : -1;  // > 0 means tilt right
            const DWORD now = info->time ? info->time : GetTickCount();
            const DWORD elapsed = hasLast ? (now - lastTriggerTime) : 999999;

            // Debounce: ignore hardware micro-switch bounce or multi-packet reports
            // within gestureGapMs, while allowing rapid direction reversals.
            if (!hasLast || dir != lastDir || elapsed >= g_settings.gestureGapMs) {
                lastTriggerTime = now;
                lastDir = dir;
                hasLast = true;
                EnqueueSwitch(dir);
            }
            if (g_settings.swallow) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void InstallHook() {
    if (!g_hook) {
        g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_instance, 0);
        Log("SetWindowsHookEx -> %p (error %lu)", g_hook, g_hook ? 0 : GetLastError());
    }
}

void RemoveHook() {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------

// Coverage test for the icon shape in normalized [0,1] coordinates:
// a rounded "desktop" rectangle flanked by left/right arrows.
bool IconShapeContains(float u, float v) {
    // Center desktop, rounded corners.
    const float halfW = 0.20f, halfH = 0.28f, radius = 0.07f;
    float dx = (u > 0.5f ? u - 0.5f : 0.5f - u) - (halfW - radius);
    float dy = (v > 0.5f ? v - 0.5f : 0.5f - v) - (halfH - radius);
    if (dx <= radius && dy <= radius) {
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        if (dx * dx + dy * dy <= radius * radius) return true;
    }
    // Arrows: tip at the outer edge, base towards the desktop.
    const float side = u < 0.5f ? u : 1.0f - u;
    const float tip = 0.02f, base = 0.25f, arrowHalfH = 0.20f;
    if (side >= tip && side <= base) {
        const float allowed = (side - tip) / (base - tip) * arrowHalfH;
        const float dv = v > 0.5f ? v - 0.5f : 0.5f - v;
        if (dv <= allowed) return true;
    }
    return false;
}

HICON CreateTrayIcon(DWORD rgb) {
    const int size = GetSystemMetrics(SM_CXSMICON);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;

    // 4x4 supersampling for smooth edges at any DPI.
    auto* pixels = static_cast<DWORD*>(bits);
    constexpr int kSub = 4;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < kSub; ++sy) {
                for (int sx = 0; sx < kSub; ++sx) {
                    const float u = (x + (sx + 0.5f) / kSub) / size;
                    const float v = (y + (sy + 0.5f) / kSub) / size;
                    if (IconShapeContains(u, v)) ++hits;
                }
            }
            const DWORD alpha = static_cast<DWORD>(hits * 255 / (kSub * kSub));
            pixels[y * size + x] = (alpha << 24) | (rgb & 0x00FFFFFF);
        }
    }

    // Monochrome AND mask: all zero, transparency comes from the alpha channel.
    const int maskStride = ((size + 15) / 16) * 2;
    BYTE* maskBits = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                  static_cast<SIZE_T>(maskStride) * size));
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    HeapFree(GetProcessHeap(), 0, maskBits);
    return icon;
}

void FillTrayData(NOTIFYICONDATAW& nid) {
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = g_settings.enabled ? g_iconActive : g_iconPaused;
    lstrcpynW(nid.szTip,
              g_settings.enabled ? L"VirtTilt - tilt wheel to switch desktops"
                                 : L"VirtTilt - paused",
              ARRAYSIZE(nid.szTip));
}

bool AddTrayIcon() {
    NOTIFYICONDATAW nid = {};
    FillTrayData(nid);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        Log("Shell_NotifyIcon(NIM_ADD) failed, error %lu", GetLastError());
        return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    const BOOL versionOk = Shell_NotifyIconW(NIM_SETVERSION, &nid);
    Log("tray icon added (icon=%p, setversion=%d)", nid.hIcon, versionOk);
    return true;
}

void UpdateTrayIcon() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid = {};
    FillTrayData(nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

void EnsureTrayIcon() {
    g_trayAdded = AddTrayIcon();
    if (g_trayAdded) {
        KillTimer(g_hwnd, kTrayRetryTimer);
    } else if (g_trayRetries < kTrayRetryMax) {
        ++g_trayRetries;
        SetTimer(g_hwnd, kTrayRetryTimer, 1000, nullptr);
    }
}

void ShowTrayMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    auto check = [](bool on) { return MF_STRING | (on ? MF_CHECKED : MF_UNCHECKED); };
    AppendMenuW(menu, check(g_settings.enabled), ID_ENABLED, L"&Enabled");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, check(g_settings.invert), ID_INVERT, L"&Reverse direction");
    AppendMenuW(menu, check(g_settings.swallow), ID_SWALLOW, L"&Block horizontal scrolling");
    AppendMenuW(menu, check(g_settings.animate), ID_ANIMATE, L"Desktop switch &animation");

    HMENU speedMenu = CreatePopupMenu();
    AppendMenuW(speedMenu, MF_STRING, ID_SPEED_FAST, L"&Fast (150 ms)");
    AppendMenuW(speedMenu, MF_STRING, ID_SPEED_NORMAL, L"&Normal (180 ms)");
    AppendMenuW(speedMenu, MF_STRING, ID_SPEED_RELAXED, L"&Relaxed (220 ms)");
    AppendMenuW(speedMenu, MF_STRING, ID_SPEED_SAFE, L"&Safe (260 ms)");

    UINT activeSpeed = ID_SPEED_NORMAL;
    if (g_settings.gestureGapMs <= 165) activeSpeed = ID_SPEED_FAST;
    else if (g_settings.gestureGapMs >= 240) activeSpeed = ID_SPEED_SAFE;
    else if (g_settings.gestureGapMs >= 200) activeSpeed = ID_SPEED_RELAXED;
    CheckMenuRadioItem(speedMenu, ID_SPEED_FAST, ID_SPEED_SAFE, activeSpeed, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(speedMenu), L"Double-tilt &speed");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MODE_API, L"Switch via &API (works in RDP)");
    AppendMenuW(menu, MF_STRING, ID_MODE_KEYS, L"Switch via &Keyboard (Ctrl+Win+Arrow)");
    CheckMenuRadioItem(menu, ID_MODE_API, ID_MODE_KEYS,
                       g_settings.useApi ? ID_MODE_API : ID_MODE_KEYS, MF_BYCOMMAND);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, check(IsAutostartEnabled()), ID_AUTOSTART, L"Start with &Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"E&xit");

    // Required so the menu closes when clicking elsewhere.
    SetForegroundWindow(g_hwnd);
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    TrackPopupMenuEx(menu, align | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, x, y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void ApplyEnabled() {
    if (g_settings.enabled) {
        InstallHook();
    } else {
        RemoveHook();
    }
    UpdateTrayIcon();
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LOG_MOUSE: {
        const DWORD mouseData = static_cast<DWORD>(lParam & 0xFFFFFFFF);
        const DWORD flags = static_cast<DWORD>(static_cast<ULONG_PTR>(lParam) >> 32);
        const char* name = "other";
        switch (wParam) {
        case WM_LBUTTONDOWN: name = "LBUTTONDOWN"; break;
        case WM_LBUTTONUP: name = "LBUTTONUP"; break;
        case WM_RBUTTONDOWN: name = "RBUTTONDOWN"; break;
        case WM_RBUTTONUP: name = "RBUTTONUP"; break;
        case WM_MBUTTONDOWN: name = "MBUTTONDOWN"; break;
        case WM_MBUTTONUP: name = "MBUTTONUP"; break;
        case WM_XBUTTONDOWN: name = "XBUTTONDOWN"; break;
        case WM_XBUTTONUP: name = "XBUTTONUP"; break;
        case WM_MOUSEWHEEL: name = "MOUSEWHEEL"; break;
        case WM_MOUSEHWHEEL: name = "MOUSEHWHEEL"; break;
        }
        Log("mouse %s (0x%04X) delta/hiword=%d flags=0x%lX", name, static_cast<UINT>(wParam),
            static_cast<short>(HIWORD(mouseData)), flags);
        return 0;
    }

    case WM_TRAY_CALLBACK:
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowTrayMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_ENABLED:
            g_settings.enabled = !g_settings.enabled;
            SaveSettings();
            ApplyEnabled();
            break;
        case ID_INVERT:
            g_settings.invert = !g_settings.invert;
            SaveSettings();
            break;
        case ID_SWALLOW:
            g_settings.swallow = !g_settings.swallow;
            SaveSettings();
            break;
        case ID_ANIMATE:
            g_settings.animate = !g_settings.animate;
            SaveSettings();
            break;
        case ID_SPEED_FAST:
            g_settings.gestureGapMs = 150;
            SaveSettings();
            break;
        case ID_SPEED_NORMAL:
            g_settings.gestureGapMs = 180;
            SaveSettings();
            break;
        case ID_SPEED_RELAXED:
            g_settings.gestureGapMs = 220;
            SaveSettings();
            break;
        case ID_SPEED_SAFE:
            g_settings.gestureGapMs = 260;
            SaveSettings();
            break;
        case ID_MODE_API:
            g_settings.useApi = true;
            SaveSettings();
            break;
        case ID_MODE_KEYS:
            g_settings.useApi = false;
            SaveSettings();
            break;
        case ID_AUTOSTART:
            SetAutostart(!IsAutostartEnabled());
            break;
        case ID_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == kTrayRetryTimer) {
            KillTimer(hwnd, kTrayRetryTimer);
            EnsureTrayIcon();
        }
        return 0;

    case WM_POWERBROADCAST:
        // Re-arm the hook after resume; Windows can drop LL hooks across sleep.
        if (wParam == PBT_APMRESUMEAUTOMATIC && g_settings.enabled) {
            RemoveHook();
            InstallHook();
        }
        return TRUE;

    case WM_DESTROY:
        RemoveHook();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    // Explorer restarted: the tray icon is gone and must be re-added.
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg != 0) {
        g_trayRetries = 0;
        EnsureTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    g_logEnabled = cmdLine && wcsstr(cmdLine, L"--log") != nullptr;
    LogEnvironment();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("another instance is already running, exiting");
        CoUninitialize();
        return 0;  // Already running.
    }

    g_instance = instance;
    LoadSettings();

    // First run: enable autostart by default. Later runs: refresh the Run entry
    // so it keeps pointing at this exe if it was moved.
    if (ReadDword(L"FirstRunDone", 0) == 0) {
        SetAutostart(true);
        SaveSettings();
        WriteDword(L"FirstRunDone", 1);
    } else if (IsAutostartEnabled()) {
        SetAutostart(true);
    }

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_iconActive = CreateTrayIcon(0x4F8EF7);  // soft blue
    g_iconPaused = CreateTrayIcon(0x8C8C8C);  // gray

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // A real (hidden) top-level window rather than HWND_MESSAGE, because
    // message-only windows do not receive the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName, WS_POPUP, 0, 0, 0, 0,
                             nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) {
        CoUninitialize();
        return 1;
    }

    InitializeCriticalSection(&g_queueCs);
    g_switchEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_workerThread = CreateThread(nullptr, 0, SwitchWorkerThread, nullptr, 0, nullptr);

    EnsureTrayIcon();
    ApplyEnabled();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_workerExit = true;
    if (g_switchEvent) SetEvent(g_switchEvent);
    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, 1000);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
    if (g_switchEvent) {
        CloseHandle(g_switchEvent);
        g_switchEvent = nullptr;
    }
    DeleteCriticalSection(&g_queueCs);

    if (g_iconActive) DestroyIcon(g_iconActive);
    if (g_iconPaused) DestroyIcon(g_iconPaused);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
