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
// ---------------------------------------------------------------------------
// GUIDs
// ---------------------------------------------------------------------------

// COM in-process server CLSID.
// Used by DllGetClassObject -- what COM activates when asked for our renderer.
// {9A3B4C5D-6E7F-8901-ABCD-EF1234567890}
DEFINE_GUID(CLSID_OpenALSpatialProvider,
    0x9a3b4c5d, 0x6e7f, 0x8901,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

// Spatial format GUID.
// This is the subkey name under Spatial\Encoder that SystemSettings.exe
// enumerates. It also gets embedded in the per-device property blobs that
// Windows Audio writes when the user activates a provider. It must be
// distinct from the COM CLSID.
// {2A7F8E1D-3C4B-5D6E-7F80-9A1B2C3D4E5F}
DEFINE_GUID(GUID_OpenALSpatialFormat,
    0x2a7f8e1d, 0x3c4b, 0x5d6e,
    0x7f, 0x80, 0x9a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f);

// ---------------------------------------------------------------------------
// Registry paths
// ---------------------------------------------------------------------------

// THE PATH SystemSettings.exe reads to build the spatial sound dropdown.
// Confirmed by Process Monitor: it calls RegOpenKey on each GUID subkey here.
// Windows Sonic's format GUID {B53D940C-B846-4831-9F76-D102B9B725A0} was
// attempted here and returned NAME NOT FOUND (Windows Sonic is hardcoded in
// the audio stack and does NOT register here -- third-party providers do).
// This path is admin-writable; no LocalSystem service required.
static const wchar_t* kEncoderPath =
    L"SOFTWARE\\Microsoft\\Multimedia\\Audio\\Spatial\\Encoder";

// Per-device selection state. Windows Audio WRITES the selected provider's
// format GUID into binary property blobs under this path when the user picks
// a provider. We do NOT write here -- Windows handles it automatically once
// the provider is selected. Kept for legacy cleanup (unregister old builds).
static const wchar_t* kMMDevicesPath =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
    L"MMDevices\\SpatialAudioEndpoint";

// COM server registration -- admin-writable, no service needed.
static const wchar_t* kCOMBase =
    L"SOFTWARE\\Classes\\CLSID";

// Service name for the MMDevices fallback writer (kept only for cleaning up
// keys written by older builds of this tool).
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
// Spatial\Encoder registration
//
// Structure (confirmed by Process Monitor + RegistryChangesView analysis):
//
//   HKLM\SOFTWARE\Microsoft\Multimedia\Audio\Spatial\Encoder\
//       {format-guid}\               <- spatial FORMAT guid, not COM CLSID
//           (default)  = "Display name shown in Settings dropdown"
//           CLSID      = "{com-clsid}"   <- maps to InProcServer32
//           IconPath   = "path\to\dll,0"
//
// The format GUID is what Windows Audio embeds in per-device property blobs
// when the user activates the provider (the bytes seen in RegistryChangesView
// for Windows Sonic are its format GUID {B53D940C...}, not its COM CLSID).
// The COM CLSID in the CLSID value is what CoCreateInstance is called with.
// ---------------------------------------------------------------------------
static bool RegisterEncoderKey(const std::wstring& dll,
                                const GUID& formatGuid,
                                const GUID& comClsid)
{
    std::wstring fmtStr  = GuidToString(formatGuid);
    std::wstring clsStr  = GuidToString(comClsid);
    std::wstring key     = std::wstring(kEncoderPath) + L"\\" + fmtStr;

    HKEY hk = nullptr;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(),
                 0, nullptr, REG_OPTION_NON_VOLATILE,
                 KEY_WRITE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) {
        wprintf(L"  [FAIL] Encoder key: error %lu (%s)\n"
                L"         Path: HKLM\\%s\n",
                (DWORD)r, Win32Msg((DWORD)r).c_str(), key.c_str());
        return false;
    }

    // Display name shown in the Settings -> Spatial audio dropdown
    const wchar_t* name = L"OpenAL Soft 3D Audio (HRTF)";
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (const BYTE*)name, (DWORD)((wcslen(name)+1)*sizeof(wchar_t)));

    // CLSID value -> COM server activated when this format is selected
    RegSetValueExW(hk, L"CLSID", 0, REG_SZ,
        (const BYTE*)clsStr.c_str(),
        (DWORD)((clsStr.size()+1)*sizeof(wchar_t)));

    // Optional icon (first resource in our DLL)
    std::wstring icon = dll + L",0";
    RegSetValueExW(hk, L"IconPath", 0, REG_SZ,
        (const BYTE*)icon.c_str(),
        (DWORD)((icon.size()+1)*sizeof(wchar_t)));

    RegCloseKey(hk);
    wprintf(L"  [OK]  Encoder key written.\n"
            L"        Path        : HKLM\\%s\n"
            L"        Format GUID : %s\n"
            L"        COM CLSID   : %s\n"
            L"        Display name: %s\n",
            key.c_str(), fmtStr.c_str(), clsStr.c_str(), name);
    return true;
}

static bool UnregisterEncoderKey(const GUID& formatGuid)
{
    std::wstring key = std::wstring(kEncoderPath) + L"\\" + GuidToString(formatGuid);
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    if (r == ERROR_SUCCESS)
        wprintf(L"  [OK]  Deleted Encoder key: HKLM\\%s\n", key.c_str());
    else if (r == ERROR_FILE_NOT_FOUND)
        wprintf(L"  [--]  Encoder key not found (already removed).\n");
    else {
        wprintf(L"  [FAIL] Encoder key: error %lu (%s)\n",
                (DWORD)r, Win32Msg((DWORD)r).c_str());
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
    wprintf(L"  Registered Spatial\\Encoder providers\n"
            L"  (HKLM\\%s):\n\n", kEncoderPath);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kEncoderPath,
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        wchar_t sub[256]; DWORD idx = 0;
        bool any = false;
        while (RegEnumKeyW(hk, idx++, sub, 256) == ERROR_SUCCESS) {
            any = true;
            std::wstring subPath = std::wstring(kEncoderPath) + L"\\" + sub;
            std::wstring disp  = RegReadStr(HKEY_LOCAL_MACHINE,
                                            subPath.c_str(), nullptr);
            std::wstring clsid = RegReadStr(HKEY_LOCAL_MACHINE,
                                            subPath.c_str(), L"CLSID");
            wprintf(L"  Format GUID : %s\n"
                    L"    Name  : %s\n"
                    L"    CLSID : %s\n\n",
                    sub,
                    disp.empty()  ? L"(no default value)" : disp.c_str(),
                    clsid.empty() ? L"(no CLSID value)"   : clsid.c_str());
        }
        if (!any) wprintf(L"  (none registered -- run 'register' first)\n");
        RegCloseKey(hk);
    } else {
        wprintf(L"  Key does not exist -- run 'register' first.\n");
    }
}

// ---------------------------------------------------------------------------
// diagnose: registry access, DLL load, CoCreateInstance
// ---------------------------------------------------------------------------
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
    wprintf(L"\n  COM InProcServer32:\n");
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
        bool fileOk = (GetFileAttributesW(dllPath.c_str()) !=
                       INVALID_FILE_ATTRIBUTES);
        wprintf(L"  DLL file : %s\n",
                fileOk ? L"found" : L"NOT FOUND -- path in registry is stale");
    }

    // ---- DLL load test ----
    wprintf(L"\n  DLL load + export test:\n");
    bool dllLoaded = false;
    if (!dllPath.empty()) {
        HMODULE hm = LoadLibraryExW(dllPath.c_str(), nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hm) {
            dllLoaded = true;
            wprintf(L"  [OK]  LoadLibraryEx succeeded.\n");
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
            wprintf(L"  [FAIL] LoadLibraryEx: error %lu (%s)\n",
                    err, Win32Msg(err).c_str());
            if (err == ERROR_MOD_NOT_FOUND) {
                wprintf(L"\n"
                    L"  CAUSE: A dependency DLL is missing (most likely OpenAL32.dll).\n"
                    L"  The Windows Audio service (audiosrv) has a restricted DLL search\n"
                    L"  path and cannot find OpenAL32.dll next to openal_spatial.dll.\n\n"
                    L"  FIX (choose one):\n"
                    L"    A) Copy OpenAL32.dll to C:\\Windows\\System32\\\n"
                    L"    B) Copy OpenAL32.dll to the same folder as openal_spatial.dll\n"
                    L"       AND ensure that folder is in the system PATH (not user PATH).\n\n"
                    L"  After fixing, re-run:\n"
                    L"    net stop audiosrv && net start audiosrv\n");
            }
        }
    }

    // ---- CoCreateInstance test ----
    wprintf(L"\n  CoCreateInstance test (what audiosrv does at provider discovery):\n");
    if (!dllPath.empty()) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUnknown* punk = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_OpenALSpatialProvider,
                                      nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IUnknown), (void**)&punk);
        if (SUCCEEDED(hr) && punk) {
            wprintf(L"  [OK]  CoCreateInstance succeeded.\n"
                    L"\n"
                    L"  COM activation works from an interactive process.\n"
                    L"  If the provider still does not appear in Sound settings,\n"
                    L"  the registry path we write to is not what Windows reads.\n"
                    L"  Run: RegisterProvider.exe procmon\n"
                    L"  to get exact Process Monitor instructions to find the real path.\n");
            punk->Release();
        } else {
            wprintf(L"  [FAIL] hr=0x%08X  ", (unsigned)hr);
            if (hr == REGDB_E_CLASSNOTREG)
                wprintf(L"CLSID not registered. Run 'register' first.\n");
            else if ((DWORD)hr == 0x8007007E || (DWORD)hr == 0x80070002)
                wprintf(L"DLL or a dependency not found (see DLL load test above).\n");
            else
                wprintf(L"%s\n", Win32Msg((DWORD)hr).c_str());

            if (!dllLoaded)
                wprintf(L"  (CoCreateInstance skipped -- DLL did not load)\n");
        }
        CoUninitialize();
    }

    wprintf(L"\n  NEXT STEP SUMMARY\n"
            L"  -----------------\n"
            L"  LoadLibraryEx FAIL  -> Copy OpenAL32.dll to System32, restart audiosrv.\n"
            L"  CoCreateInstance OK -> Registry path wrong. Run: RegisterProvider.exe procmon\n"
            L"  CoCreateInstance FAIL (DLL loaded) -> Bug in DllGetClassObject.\n");
}

// ---------------------------------------------------------------------------
// procmon: print exact Process Monitor instructions for finding the real path
// ---------------------------------------------------------------------------
static void PrintProcMonInstructions()
{
    wprintf(
        L"  HOW TO FIND THE REAL SPATIAL AUDIO REGISTRY PATH\n"
        L"  ==================================================\n\n"
        L"  Windows does not document where it reads spatial sound providers from.\n"
        L"  Process Monitor will show us exactly which registry keys it reads\n"
        L"  when the Sound settings dropdown is opened.\n\n"
        L"  STEPS:\n\n"
        L"  1. Download Process Monitor (procmon.exe) from:\n"
        L"       https://learn.microsoft.com/sysinternals/downloads/procmon\n\n"
        L"  2. Run procmon.exe as Administrator.\n\n"
        L"  3. In the Filter menu -> Filter (Ctrl+L), add these filters:\n"
        L"       Process Name  is  SystemSettings.exe  -> Include\n"
        L"       Operation     is  RegOpenKey          -> Include\n"
        L"       Operation     is  RegQueryValue       -> Include\n"
        L"       Operation     is  RegEnumKey          -> Include\n"
        L"       Path          contains  Spatial       -> Include\n"
        L"       Path          contains  MMDevice      -> Include\n"
        L"     Click Add after each, then OK.\n\n"
        L"  4. Clear the event list (Ctrl+X).\n\n"
        L"  5. Open Settings -> System -> Sound -> [your headphones]\n"
        L"     -> Spatial audio  (open the dropdown)\n\n"
        L"  6. Stop capture (Ctrl+E).\n\n"
        L"  7. Look for RegEnumKey or RegOpenKey calls that enumerate a key\n"
        L"     whose subkeys include known CLSIDs like:\n"
        L"       {B2B88BE7-07A8-4BC7-B25B-6A68B9B0EE93}  (Windows Sonic)\n"
        L"     The parent path of that CLSID is the real provider list key.\n\n"
        L"  8. Share the captured path and we can update RegisterProvider.exe\n"
        L"     to write to the correct location.\n\n"
        L"  ALTERNATIVE: check if a Dolby Atmos trial is available in the\n"
        L"  Microsoft Store. Installing it and then examining what registry\n"
        L"  keys it creates (with procmon during install, or regshot before/after)\n"
        L"  will reveal the exact format Windows requires.\n");
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
                L"  RegisterProvider.exe  diagnose\n"
                L"  RegisterProvider.exe  procmon\n\n"
                L"'diagnose' checks COM activation and DLL loading.\n"
                L"'procmon'  prints instructions for finding the real registry path\n"
                L"           Windows uses for the spatial sound dropdown.\n"
                L"Run as Administrator.\n");
        return 1;
    }

    std::wstring cmd = argv[1];

    if (cmd == L"diagnose") { Diagnose(); return 0; }
    if (cmd == L"procmon")  { PrintProcMonInstructions(); return 0; }
    if (cmd == L"list")     { ListProviders(); return 0; }

    if (cmd == L"unregister") {
        wprintf(L"[Step 1] Removing COM server (InProcServer32)...\n");
        UnregisterCOMServer(CLSID_OpenALSpatialProvider);

        wprintf(L"\n[Step 2] Removing Spatial\\Encoder key...\n");
        UnregisterEncoderKey(GUID_OpenALSpatialFormat);

        // Step 3: Clean up any MMDevices key written by older builds of this
        // tool. The LocalSystem service is still needed here because we wrote
        // to a SYSTEM-protected path in previous versions. Non-fatal if it
        // fails (key may not exist on clean installs of the new build).
        wprintf(L"\n[Step 3] Cleaning up legacy MMDevices key (if any)...\n");
        if (!RunAsService(L"delete", L"", CLSID_OpenALSpatialProvider)) {
            wprintf(L"  [--]  Legacy MMDevices key not found or already removed.\n");
        } else {
            wprintf(L"  [OK]  Legacy MMDevices key removed.\n");
        }

        wprintf(L"\nUnregistered. Restart the audio service:\n"
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

        // Step 1: COM InProcServer32 entry (admin-writable)
        wprintf(L"[Step 1] Registering COM server (InProcServer32)...\n");
        if (!RegisterCOMServer(dll, CLSID_OpenALSpatialProvider)) return 1;

        // Step 2: Spatial\Encoder key -- what SystemSettings.exe reads to
        // build the spatial sound dropdown. Admin-writable, no service needed.
        // The subkey name is the spatial FORMAT GUID (not the COM CLSID).
        // The CLSID value inside it maps to our InProcServer32 entry above.
        wprintf(L"\n[Step 2] Writing Spatial\\Encoder provider key...\n");
        if (!RegisterEncoderKey(dll,
                                GUID_OpenALSpatialFormat,
                                CLSID_OpenALSpatialProvider)) return 1;

        wprintf(L"\nRegistration complete!\n\n"
                L"To activate:\n"
                L"  1. Restart the audio service:\n"
                L"       net stop audiosrv && net start audiosrv\n"
                L"  2. Open Settings -> System -> Sound -> [your output device]\n"
                L"     -> Spatial audio  (or right-click the tray speaker icon\n"
                L"        -> Spatial sound)\n"
                L"     'OpenAL Soft 3D Audio (HRTF)' should now appear.\n\n"
                L"If it does not appear, run:\n"
                L"  RegisterProvider.exe diagnose\n");
        return 0;
    }

    wprintf(L"Unknown command: %s\n", cmd.c_str());
    return 1;
}
