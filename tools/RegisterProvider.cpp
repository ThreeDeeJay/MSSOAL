/**
 * RegisterProvider.cpp
 * ============================================================
 * Registers/unregisters OpenAL Soft as a Windows Spatial Sound
 * provider visible in the Sound control panel alongside
 * Windows Sonic and Dolby Atmos.
 *
 * Usage (elevated prompt required):
 *   RegisterProvider.exe  register   [path\to\openal_spatial.dll]
 *   RegisterProvider.exe  unregister
 *   RegisterProvider.exe  list
 *   RegisterProvider.exe  diagnose
 *
 * HOW IT WORKS
 * ------------
 * Windows reads spatial sound providers from:
 *   HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\
 *       MMDevices\SpatialAudioEndpoint\{clsid}\
 *
 * That subtree has SYSTEM-level ACLs -- even a true Administrator
 * token is denied write access.  To work around this (exactly as
 * Dolby Atmos and DTS do) we install a one-shot Windows service
 * that runs as LocalSystem, writes the keys, then uninstalls
 * itself.  The service is this same executable called with the
 * hidden "svc-write" or "svc-delete" verb.
 * ============================================================
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Our provider CLSID {9A3B4C5D-6E7F-8901-ABCD-EF1234567890}
// ---------------------------------------------------------------------------
DEFINE_GUID(CLSID_OpenALSpatialProvider,
    0x9a3b4c5d, 0x6e7f, 0x8901,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

// ---------------------------------------------------------------------------
// Registry paths
// ---------------------------------------------------------------------------
// The path Windows actually reads for the Sound control panel dropdown.
// Protected by SYSTEM ACLs; we write here from a LocalSystem service.
static const wchar_t* kMMDevicesPath =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
    L"MMDevices\\SpatialAudioEndpoint";

// COM server registration -- admin-writable, no service needed.
static const wchar_t* kCOMBase =
    L"SOFTWARE\\Classes\\CLSID";

// Service name used for the one-shot SYSTEM helper
static const wchar_t* kSvcName = L"OALSpatialRegHelper";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring Win32Msg(DWORD err)
{
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, buf, 512, nullptr);
    size_t n = wcslen(buf);
    while (n > 0 && (buf[n-1]=='\r'||buf[n-1]=='\n')) buf[--n] = 0;
    return buf;
}

static std::wstring GuidToString(const GUID& g)
{
    wchar_t buf[64]; StringFromGUID2(g, buf, 64); return buf;
}

static bool IsElevated()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{}; DWORD sz = sizeof(e);
    bool el = GetTokenInformation(tok, TokenElevation, &e, sz, &sz)
              && e.TokenIsElevated;
    CloseHandle(tok);
    return el;
}

// Attempt to read a REG_SZ value; returns empty string on failure.
static std::wstring RegReadStr(HKEY root, const wchar_t* path,
                               const wchar_t* value)
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return {};
    wchar_t buf[512] = {}; DWORD sz = sizeof(buf);
    RegQueryValueExW(hk, value, nullptr, nullptr, (BYTE*)buf, &sz);
    RegCloseKey(hk);
    return buf;
}

// ---------------------------------------------------------------------------
// COM server registration (admin-writable, no service required)
// ---------------------------------------------------------------------------
static bool RegisterCOMServer(const std::wstring& dll, const GUID& clsid)
{
    std::wstring cs  = GuidToString(clsid);
    std::wstring key = std::wstring(kCOMBase) + L"\\" + cs;

    HKEY hk = nullptr;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(),
                 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) {
        wprintf(L"  [FAIL] CLSID key: error %lu %s\n", (DWORD)r,
                Win32Msg((DWORD)r).c_str());
        return false;
    }
    const wchar_t* name = L"OpenAL Soft Spatial Audio Renderer";
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (const BYTE*)name, (DWORD)((wcslen(name)+1)*sizeof(wchar_t)));
    RegCloseKey(hk);

    std::wstring ip = key + L"\\InProcServer32";
    r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ip.c_str(),
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) {
        wprintf(L"  [FAIL] InProcServer32 key: error %lu %s\n", (DWORD)r,
                Win32Msg((DWORD)r).c_str());
        return false;
    }
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (const BYTE*)dll.c_str(),
        (DWORD)((dll.size()+1)*sizeof(wchar_t)));
    const wchar_t* model = L"Both";
    RegSetValueExW(hk, L"ThreadingModel", 0, REG_SZ,
        (const BYTE*)model, (DWORD)((wcslen(model)+1)*sizeof(wchar_t)));
    RegCloseKey(hk);

    wprintf(L"  [OK]  COM server: HKLM\\%s\n", key.c_str());
    return true;
}

static bool UnregisterCOMServer(const GUID& clsid)
{
    std::wstring key = std::wstring(kCOMBase) + L"\\" + GuidToString(clsid);
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    if (r == ERROR_SUCCESS)
        wprintf(L"  [OK]  Deleted COM key: HKLM\\%s\n", key.c_str());
    else if (r == ERROR_FILE_NOT_FOUND)
        wprintf(L"  [--]  COM key not found.\n");
    else {
        wprintf(L"  [FAIL] COM key: error %lu %s\n", (DWORD)r,
                Win32Msg((DWORD)r).c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The actual MMDevices write -- called from within the LocalSystem service.
// These functions run with SYSTEM privileges.
// ---------------------------------------------------------------------------
// Enable a named privilege in the current token.
// Takes a narrow string (SE_BACKUP_NAME / SE_RESTORE_NAME are char* in the SDK).
// Required before using REG_OPTION_BACKUP_RESTORE.
static DWORD EnablePrivilege(const char* name)
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return GetLastError();

    LUID luid{};
    if (!LookupPrivilegeValueA(nullptr, name, &luid)) {
        DWORD err = GetLastError(); CloseHandle(tok); return err;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();  // ERROR_SUCCESS or ERROR_NOT_ALL_ASSIGNED
    CloseHandle(tok);
    return err;
}

// Write the SpatialAudioEndpoint subkey for our CLSID.
// Returns the Win32 error code directly (ERROR_SUCCESS on success) so
// SvcMain can pass the real value as the service exit code instead of
// the opaque ERROR_FUNCTION_FAILED (1627).
//
// Key technique: REG_OPTION_BACKUP_RESTORE with SeRestorePrivilege
// ----------------------------------------------------------------
// Even LocalSystem cannot create subkeys under MMDevices using normal
// KEY_WRITE access because the key is owned by TrustedInstaller with a
// DENY ACE for everyone else.  REG_OPTION_BACKUP_RESTORE bypasses the
// DACL entirely when SE_BACKUP_NAME / SE_RESTORE_NAME are active --
// this is exactly how regedit.exe "Take Ownership" works internally.
static DWORD WriteMMDevicesKey(const std::wstring& dll, const GUID& clsid)
{
    // Enable the privileges required for REG_OPTION_BACKUP_RESTORE.
    // Ignore return values -- if we lack them the RegCreateKeyEx below will
    // fail with a meaningful error code that propagates to the caller.
    EnablePrivilege(SE_BACKUP_NAME);    // L"SeBackupPrivilege"
    EnablePrivilege(SE_RESTORE_NAME);   // L"SeRestorePrivilege"

    std::wstring cs  = GuidToString(clsid);
    std::wstring key = std::wstring(kMMDevicesPath) + L"\\" + cs;

    HKEY hk = nullptr;
    // REG_OPTION_BACKUP_RESTORE: ignores samDesired, uses privilege-based
    // access instead of DACL-based access.  With SeRestorePrivilege active
    // this grants KEY_ALL_ACCESS regardless of the key's security descriptor.
    LONG r = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, key.c_str(),
        0, nullptr,
        REG_OPTION_NON_VOLATILE | REG_OPTION_BACKUP_RESTORE,
        KEY_WRITE,       // ignored when BACKUP_RESTORE is set, but required
        nullptr, &hk, nullptr);

    if (r != ERROR_SUCCESS) {
        // Log to Application event log with the real error code so it shows
        // up in Event Viewer even if the console output is not visible.
        HANDLE hEvt = RegisterEventSourceW(nullptr, L"Application");
        if (hEvt) {
            wchar_t msg[512];
            swprintf_s(msg,
                L"OALSpatialReg WriteMMDevicesKey: RegCreateKeyExW(\"%s\") "
                L"failed with error %lu", key.c_str(), (DWORD)r);
            const wchar_t* msgs[] = { msg };
            ReportEventW(hEvt, EVENTLOG_ERROR_TYPE, 0, 0,
                         nullptr, 1, 0, msgs, nullptr);
            DeregisterEventSource(hEvt);
        }
        return (DWORD)r;
    }

    const wchar_t* disp = L"OpenAL Soft 3D Audio (HRTF)";
    RegSetValueExW(hk, L"DisplayName", 0, REG_SZ,
        (const BYTE*)disp, (DWORD)((wcslen(disp)+1)*sizeof(wchar_t)));

    std::wstring icon = dll + L",0";
    RegSetValueExW(hk, L"IconPath", 0, REG_SZ,
        (const BYTE*)icon.c_str(),
        (DWORD)((icon.size()+1)*sizeof(wchar_t)));

    DWORD objTypes = 0x1FFFF;
    RegSetValueExW(hk, L"StaticObjectTypeMask", 0, REG_DWORD,
        (const BYTE*)&objTypes, sizeof(DWORD));

    DWORD maxDyn = 256;
    RegSetValueExW(hk, L"MaxDynamicObjectCount", 0, REG_DWORD,
        (const BYTE*)&maxDyn, sizeof(DWORD));

    DWORD flags = 0;
    RegSetValueExW(hk, L"Flags", 0, REG_DWORD,
        (const BYTE*)&flags, sizeof(DWORD));

    RegCloseKey(hk);

    // Log success too so it's visible in Event Viewer
    HANDLE hEvt = RegisterEventSourceW(nullptr, L"Application");
    if (hEvt) {
        wchar_t msg[512];
        swprintf_s(msg, L"OALSpatialReg: successfully wrote HKLM\\%s",
                   key.c_str());
        const wchar_t* msgs[] = { msg };
        ReportEventW(hEvt, EVENTLOG_INFORMATION_TYPE, 0, 0,
                     nullptr, 1, 0, msgs, nullptr);
        DeregisterEventSource(hEvt);
    }
    return ERROR_SUCCESS;
}

// (DeleteMMDevicesKey is handled inline in SvcMain with privilege elevation)

// ---------------------------------------------------------------------------
// One-shot service implementation
//
// The service main runs with LocalSystem privileges, performs the registry
// operation embedded in its service name via a shared memory block, then
// stops.  The SCM entry is cleaned up by the calling process once the
// service exits.
// ---------------------------------------------------------------------------

// Shared-memory layout written by the launcher, read by the service worker.
struct SvcPayload {
    wchar_t verb[32];          // L"write" or L"delete"
    wchar_t dll[MAX_PATH];     // absolute DLL path (for "write")
    GUID    clsid;
};

// Services run in Session 0; the launcher runs in the interactive session.
// "Local\" shared memory is session-scoped, so Session 0 cannot open it.
// "Global\" crosses the session boundary -- LocalSystem has the required
// SeCreateGlobalPrivilege to open Global objects unconditionally.
static const wchar_t* kShmName = L"Global\\OALSpatialRegPayload";

static SERVICE_STATUS_HANDLE g_svcHandle = nullptr;
static void SetSvcState(DWORD state, DWORD exitCode = NO_ERROR)
{
    SERVICE_STATUS ss{};
    ss.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    ss.dwCurrentState = state;
    ss.dwWin32ExitCode = exitCode;
    if (state == SERVICE_RUNNING)
        ss.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    SetServiceStatus(g_svcHandle, &ss);
}

static VOID WINAPI SvcCtrl(DWORD) {}

static VOID WINAPI SvcMain(DWORD, LPWSTR*)
{
    g_svcHandle = RegisterServiceCtrlHandlerW(kSvcName, SvcCtrl);
    if (!g_svcHandle) return;
    SetSvcState(SERVICE_RUNNING);

    // Open the shared memory written by the launcher.
    // Must use Global\ prefix -- services run in Session 0 and cannot
    // access Local\ objects created in the interactive user session.
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kShmName);
    if (!hMap) {
        DWORD err = GetLastError();
        // Write to the Application event log so the caller can diagnose
        HANDLE hEvt = RegisterEventSourceW(nullptr, L"Application");
        if (hEvt) {
            wchar_t msg[256];
            swprintf_s(msg, L"OALSpatialReg: OpenFileMapping(%s) failed: %lu",
                       kShmName, err);
            const wchar_t* msgs[] = { msg };
            ReportEventW(hEvt, EVENTLOG_ERROR_TYPE, 0, 0,
                         nullptr, 1, 0, msgs, nullptr);
            DeregisterEventSource(hEvt);
        }
        SetSvcState(SERVICE_STOPPED, err);
        return;
    }

    const SvcPayload* p = (const SvcPayload*)MapViewOfFile(
        hMap, FILE_MAP_READ, 0, 0, sizeof(SvcPayload));
    if (!p) {
        DWORD err = GetLastError();
        CloseHandle(hMap);
        SetSvcState(SERVICE_STOPPED, err);
        return;
    }

    DWORD rc = NO_ERROR;
    if (wcscmp(p->verb, L"write") == 0) {
        rc = WriteMMDevicesKey(p->dll, p->clsid);  // real Win32 error code
    } else if (wcscmp(p->verb, L"delete") == 0) {
        EnablePrivilege(SE_BACKUP_NAME);
        EnablePrivilege(SE_RESTORE_NAME);
        // Must open the PARENT key with REG_OPTION_BACKUP_RESTORE first.
        // RegDeleteTreeW(HKLM, fullPath) re-opens the parent via normal DACL
        // checks and hits ERROR_ACCESS_DENIED even with privileges active.
        // Opening the parent explicitly with REG_OPTION_BACKUP_RESTORE and
        // then deleting the child subkey from that handle bypasses the DACL.
        HKEY hParent = nullptr;
        LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kMMDevicesPath,
                     REG_OPTION_BACKUP_RESTORE, KEY_WRITE, &hParent);
        if (r == ERROR_SUCCESS) {
            std::wstring cs = GuidToString(p->clsid);
            r = RegDeleteTreeW(hParent, cs.c_str());
            RegCloseKey(hParent);
        }
        rc = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND)
             ? ERROR_SUCCESS : (DWORD)r;
    } else {
        rc = ERROR_INVALID_PARAMETER;
    }

    UnmapViewOfFile(p);
    CloseHandle(hMap);
    SetSvcState(SERVICE_STOPPED, rc);
}

// ---------------------------------------------------------------------------
// Launcher: installs the service, waits for it to finish, removes it.
// The calling process must be elevated (Administrator).
// ---------------------------------------------------------------------------
static bool RunAsService(const wchar_t* verb,
                         const std::wstring& dll,
                         const GUID& clsid)
{
    // 1. Write payload into named shared memory
    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SvcPayload), kShmName);
    if (!hMap) {
        wprintf(L"  [FAIL] CreateFileMapping: %lu\n", GetLastError());
        return false;
    }
    SvcPayload* p = (SvcPayload*)MapViewOfFile(
        hMap, FILE_MAP_WRITE, 0, 0, sizeof(SvcPayload));
    if (!p) { CloseHandle(hMap); return false; }

    wcsncpy_s(p->verb,  verb,       _TRUNCATE);
    wcsncpy_s(p->dll,   dll.c_str(), _TRUNCATE);
    p->clsid = clsid;
    UnmapViewOfFile(p);

    // 2. Build the service binary path (this executable + hidden verb)
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring binPath = L"\"";
    binPath += exePath;
    binPath += L"\" svc-run";

    // 3. Open SCM and create service
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        DWORD err = GetLastError();
        wprintf(L"  [FAIL] OpenSCManager: %lu %s\n", err, Win32Msg(err).c_str());
        CloseHandle(hMap);
        return false;
    }

    // Delete any leftover from a previous failed run
    SC_HANDLE old = OpenServiceW(scm, kSvcName, DELETE);
    if (old) { DeleteService(old); CloseServiceHandle(old); }

    SC_HANDLE svc = CreateServiceW(
        scm, kSvcName, L"OAL Spatial Audio Registration Helper",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL,
        binPath.c_str(),
        nullptr, nullptr, nullptr,
        nullptr,  // LocalSystem account
        nullptr);

    if (!svc) {
        DWORD err = GetLastError();
        wprintf(L"  [FAIL] CreateService: %lu %s\n", err, Win32Msg(err).c_str());
        CloseServiceHandle(scm);
        CloseHandle(hMap);
        return false;
    }

    // 4. Start the service
    bool ok = false;
    if (!StartServiceW(svc, 0, nullptr)) {
        wprintf(L"  [FAIL] StartService: %lu %s\n",
                GetLastError(), Win32Msg(GetLastError()).c_str());
    } else {
        // 5. Poll until stopped (should be < 1 second)
        SERVICE_STATUS ss{};
        for (int i = 0; i < 50; ++i) {
            Sleep(100);
            QueryServiceStatus(svc, &ss);
            if (ss.dwCurrentState == SERVICE_STOPPED) break;
        }
        ok = (ss.dwCurrentState == SERVICE_STOPPED &&
              ss.dwWin32ExitCode == NO_ERROR);
        if (!ok) {
            wprintf(L"  [FAIL] Service stopped: state=%lu exitCode=%lu (%s)\n"
                    L"         Check Event Viewer -> Windows Logs -> Application\n"
                    L"         filter Source = 'Application', look for OALSpatialReg.\n",
                    ss.dwCurrentState, ss.dwWin32ExitCode,
                    Win32Msg(ss.dwWin32ExitCode).c_str());
        }
    }

    // 6. Clean up service entry regardless of success
    DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    CloseHandle(hMap);
    return ok;
}

// ---------------------------------------------------------------------------
// List / diagnose
// ---------------------------------------------------------------------------
static void ListProviders()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kMMDevicesPath,
            0, KEY_READ, &hk) != ERROR_SUCCESS) {
        wprintf(L"  Cannot read HKLM\\%s\n  (no providers registered yet)\n",
                kMMDevicesPath);
        return;
    }
    wprintf(L"  Registered spatial sound providers\n"
            L"  (HKLM\\%s):\n\n", kMMDevicesPath);
    wchar_t sub[256]; DWORD idx = 0;
    while (RegEnumKeyW(hk, idx++, sub, 256) == ERROR_SUCCESS) {
        std::wstring subPath = std::wstring(kMMDevicesPath) + L"\\" + sub;
        std::wstring disp = RegReadStr(HKEY_LOCAL_MACHINE,
                                       subPath.c_str(), L"DisplayName");
        wprintf(L"  %s\n    -> %s\n", sub,
                disp.empty() ? L"(no DisplayName)" : disp.c_str());
    }
    RegCloseKey(hk);
}

static void Diagnose()
{
    wprintf(L"  Elevation : %s\n\n",
        IsElevated() ? L"YES (elevated)" : L"NO  -- re-run as Administrator");

    // ---- Registry access ----
    struct Test { const wchar_t* label; const wchar_t* path; REGSAM access; };
    Test tests[] = {
        { L"MMDevices (read) ", kMMDevicesPath, KEY_READ  },
        { L"MMDevices (write)", kMMDevicesPath, KEY_WRITE },
        { L"COM Classes(write)", kCOMBase,      KEY_WRITE },
    };
    wprintf(L"  Registry access:\n");
    for (auto& t : tests) {
        HKEY hk = nullptr;
        LONG r  = RegOpenKeyExW(HKEY_LOCAL_MACHINE, t.path, 0, t.access, &hk);
        if (r == ERROR_SUCCESS) {
            wprintf(L"  %-24s: accessible\n", t.label);
            RegCloseKey(hk);
        } else {
            wprintf(L"  %-24s: error %lu (%s)\n",
                    t.label, (DWORD)r, Win32Msg((DWORD)r).c_str());
        }
    }

    // ---- COM registration ----
    wprintf(L"\n  COM InProcServer32 registered:\n");
    wchar_t clsidStr[64] = {};
    StringFromGUID2(CLSID_OpenALSpatialProvider, clsidStr, 64);
    std::wstring inprocPath = std::wstring(kCOMBase) + L"\\" + clsidStr
                            + L"\\InProcServer32";
    std::wstring dllPath = RegReadStr(HKEY_LOCAL_MACHINE,
                                      inprocPath.c_str(), nullptr);
    if (dllPath.empty()) {
        wprintf(L"  [MISSING] Run 'register' first.\n");
    } else {
        wprintf(L"  DLL path : %s\n", dllPath.c_str());
        // Check the DLL file itself exists
        bool fileOk = (GetFileAttributesW(dllPath.c_str()) !=
                       INVALID_FILE_ATTRIBUTES);
        wprintf(L"  DLL file : %s\n",
                fileOk ? L"found" : L"NOT FOUND -- path in registry is wrong");
    }

    // ---- DLL load test ----
    // audiosrv loads the DLL in a restricted environment. Test here whether
    // all dependencies resolve -- this is the most common silent failure.
    wprintf(L"\n  DLL load test (simulates what audiosrv does):\n");
    if (!dllPath.empty()) {
        // LOAD_LIBRARY_AS_DATAFILE would not resolve imports; we want a full load.
        HMODULE hm = LoadLibraryExW(dllPath.c_str(), nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hm) {
            wprintf(L"  [OK]  LoadLibraryEx succeeded.\n");
            // Verify the required exports are present
            const char* exports[] = {
                "DllGetClassObject", "DllCanUnloadNow",
                "DllRegisterServer", "DllUnregisterServer"
            };
            for (auto* exp : exports) {
                bool found = (GetProcAddress(hm, exp) != nullptr);
                wprintf(L"  %-26S: %s\n", exp,
                        found ? L"exported" : L"MISSING -- COM will fail");
            }
            FreeLibrary(hm);
        } else {
            DWORD err = GetLastError();
            wprintf(L"  [FAIL] LoadLibraryEx error %lu (%s)\n",
                    err, Win32Msg(err).c_str());
            // Error 126 = "The specified module could not be found"
            // This means a DEPENDENCY DLL is missing, not the DLL itself.
            if (err == ERROR_MOD_NOT_FOUND) {
                wprintf(L"\n  >>> Most likely cause: OpenAL32.dll (or soft_oal.dll)\n"
                        L"  >>> is not in a directory that audiosrv can search.\n"
                        L"  >>> Fix: copy OpenAL32.dll to the same folder as\n"
                        L"  >>>   openal_spatial.dll, OR copy it to:\n"
                        L"  >>>   C:\\Windows\\System32\\OpenAL32.dll\n"
                        L"  >>> Then re-run: net stop audiosrv && net start audiosrv\n");
            }
        }
    }

    // ---- CoCreateInstance test ----
    // If the DLL loaded above but CoCreateInstance still fails, the class
    // factory has a bug.  If it succeeds here but not in audiosrv, the issue
    // is the service's restricted token or missing dependency in Session 0.
    wprintf(L"\n  CoCreateInstance test (ISpatialAudioClient):\n");
    if (!dllPath.empty()) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUnknown* punk = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_OpenALSpatialProvider,
                                      nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IUnknown), (void**)&punk);
        if (SUCCEEDED(hr) && punk) {
            wprintf(L"  [OK]  CoCreateInstance succeeded -- COM activation works.\n");
            punk->Release();
        } else {
            wprintf(L"  [FAIL] hr=0x%08X  ", (unsigned)hr);
            if (hr == REGDB_E_CLASSNOTREG)
                wprintf(L"CLSID not registered -- run 'register' first.\n");
            else if (hr == (HRESULT)0x8007007E || hr == (HRESULT)0x80070002)
                wprintf(L"DLL or dependency not found (error 126/2).\n"
                        L"  Copy OpenAL32.dll to System32 or beside openal_spatial.dll.\n");
            else
                wprintf(L"%s\n", Win32Msg((DWORD)hr).c_str());
        }
        CoUninitialize();
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);

    // Hidden verb: service worker entry point (called by SCM, not the user)
    if (argc >= 2 && wcscmp(argv[1], L"svc-run") == 0) {
        SERVICE_TABLE_ENTRYW tbl[] = {
            { const_cast<LPWSTR>(kSvcName), SvcMain },
            { nullptr, nullptr }
        };
        StartServiceCtrlDispatcherW(tbl);
        return 0;
    }

    wprintf(L"OpenAL Spatial Audio - Windows Spatial Sound Provider Registration\n"
            L"===================================================================\n\n");

    if (!IsElevated()) {
        wprintf(L"WARNING: Not elevated. Registry writes will fail.\n"
                L"Right-click your terminal and choose 'Run as Administrator'.\n\n");
    }

    if (argc < 2) {
        wprintf(L"Usage:\n"
                L"  RegisterProvider.exe  register   [path\\to\\openal_spatial.dll]\n"
                L"  RegisterProvider.exe  unregister\n"
                L"  RegisterProvider.exe  list\n"
                L"  RegisterProvider.exe  diagnose\n\n"
                L"Registers the provider in the MMDevices\\SpatialAudioEndpoint subtree\n"
                L"so it appears in the Sound control panel spatial sound dropdown.\n"
                L"Run as Administrator (a one-shot LocalSystem service handles the\n"
                L"SYSTEM-ACL-protected MMDevices key automatically).\n");
        return 1;
    }

    std::wstring cmd = argv[1];

    if (cmd == L"diagnose") { Diagnose(); return 0; }
    if (cmd == L"list")     { ListProviders(); return 0; }

    if (cmd == L"unregister") {
        wprintf(L"[Step 1] Removing COM server...\n");
        UnregisterCOMServer(CLSID_OpenALSpatialProvider);

        wprintf(L"\n[Step 2] Removing MMDevices key via LocalSystem service...\n");
        if (!RunAsService(L"delete", L"", CLSID_OpenALSpatialProvider)) {
            wprintf(L"  [FAIL] Service-based removal failed.\n");
            return 1;
        }
        wprintf(L"  [OK]  MMDevices key removed.\n");
        wprintf(L"\nUnregistered. Restart the audio service to take effect:\n"
                L"  net stop audiosrv && net start audiosrv\n");
        return 0;
    }

    if (cmd == L"register") {
        std::wstring dll;
        if (argc >= 3) {
            dll = argv[2];
        } else {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            dll = exePath;
            size_t sl = dll.rfind(L'\\');
            if (sl != std::wstring::npos) dll.resize(sl + 1);
            dll += L"openal_spatial.dll";
        }

        // Resolve to absolute path so the registry value is stable
        wchar_t abs[MAX_PATH] = {};
        if (!GetFullPathNameW(dll.c_str(), MAX_PATH, abs, nullptr) ||
            !PathFileExistsW(abs)) {
            wprintf(L"  [FAIL] DLL not found: %s\n"
                    L"  Provide the correct path as the second argument.\n", dll.c_str());
            return 1;
        }
        dll = abs;
        wprintf(L"  DLL: %s\n\n", dll.c_str());

        // Step 1: COM registration (admin-writable, no service needed)
        wprintf(L"[Step 1] Registering COM server...\n");
        if (!RegisterCOMServer(dll, CLSID_OpenALSpatialProvider)) return 1;

        // Step 2: MMDevices key via LocalSystem service
        wprintf(L"\n[Step 2] Writing MMDevices\\SpatialAudioEndpoint key\n"
                L"         via one-shot LocalSystem service...\n");
        if (!RunAsService(L"write", dll, CLSID_OpenALSpatialProvider)) {
            wprintf(L"  [FAIL] Service-based registration failed.\n"
                    L"  Run 'RegisterProvider.exe diagnose' for details.\n");
            return 1;
        }
        wprintf(L"  [OK]  MMDevices\\SpatialAudioEndpoint key written.\n");

        wprintf(L"\nRegistration complete!\n\n"
                L"To activate:\n"
                L"  1. Restart the audio service:\n"
                L"       net stop audiosrv && net start audiosrv\n"
                L"  2. Right-click the speaker in the tray -> Spatial sound\n"
                L"     'OpenAL Soft 3D Audio (HRTF)' should now appear.\n"
                L"  3. Alternatively select it from:\n"
                L"       Settings -> System -> Sound -> [your headphones]\n"
                L"       -> Spatial audio\n\n"
                L"To verify:\n"
                L"  RegisterProvider.exe list\n");
        return 0;
    }

    wprintf(L"Unknown command: %s\n", cmd.c_str());
    return 1;
}
