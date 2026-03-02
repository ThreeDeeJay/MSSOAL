/**
 * RegisterProvider.cpp
 * ============================================================
 * Registers/unregisters OpenALSpatialAudio as a Windows Spatial
 * Sound COM provider.
 *
 * Usage (run as Administrator):
 *   RegisterProvider.exe  register   [path\to\openal_spatial.dll]
 *   RegisterProvider.exe  unregister
 *   RegisterProvider.exe  list
 *   RegisterProvider.exe  diagnose
 *
 * Registry layout written:
 *
 *   HKLM\SOFTWARE\Classes\CLSID\{our-clsid}\
 *       (default)         = "OpenAL Soft Spatial Audio Renderer"
 *       InProcServer32\
 *           (default)     = path\to\openal_spatial.dll
 *           ThreadingModel= "Both"
 *
 *   HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\
 *       Audio\SpatialRendering\{our-clsid}\
 *           DisplayName           = "OpenAL Soft 3D Audio (HRTF)"
 *           StaticObjectTypeMask  = DWORD
 *           MaxDynamicObjectCount = DWORD
 *
 * NOTE on MMDevices:
 *   HKLM\...\MMDevices is protected by SYSTEM-level ACLs. Even
 *   a true Administrator token cannot write to that subtree
 *   without first taking ownership via SetNamedSecurityInfo().
 *   We therefore write to the Audio\SpatialRendering path instead,
 *   which standard admin tokens can create. The diagnose command
 *   shows whether MMDevices is accessible on this machine.
 * ============================================================
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// initguid.h must precede DEFINE_GUID to emit definitions, not extern decls
#include <initguid.h>
#include <shlwapi.h>
#include <aclapi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

// - Embed a UAC manifest so Windows enforces elevation before launch -
// This pragma causes the linker to embed the manifest resource directly,
// removing any "need Administrator?" ambiguity at runtime.
#pragma comment(linker, \
    "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

// -
// Our provider CLSID  {9A3B4C5D-6E7F-8901-ABCD-EF1234567890}
// Generate a fresh GUID with uuidgen.exe for production use.
// -
DEFINE_GUID(CLSID_OpenALSpatialProvider,
    0x9a3b4c5d, 0x6e7f, 0x8901,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

// Spatial provider key - under Audio\SpatialRendering (admin-writable)
static const wchar_t* kSpatialKeyBase =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
    L"Audio\\SpatialRendering";

// MMDevices path - SYSTEM-protected, shown in diagnose only
static const wchar_t* kMMDevicesBase =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
    L"MMDevices\\SpatialAudioEndpoint";

static const wchar_t* kCOMServerBase =
    L"SOFTWARE\\Classes\\CLSID";

// -
// Helpers
// -

// Format a Win32 error code as a readable string
static std::wstring Win32Msg(DWORD err)
{
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buf, 512, nullptr);
    // Strip trailing newline
    size_t n = wcslen(buf);
    while (n > 0 && (buf[n-1] == L'\r' || buf[n-1] == L'\n')) buf[--n] = 0;
    return buf;
}

static std::wstring GuidToString(const GUID& g)
{
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return buf;
}

// Create a registry key and every intermediate component.
// Returns ERROR_SUCCESS or a Win32 error code.
static LONG CreateKeyPath(HKEY root, const wchar_t* path,
                           REGSAM access, HKEY* out)
{
    // RegCreateKeyExW already creates intermediate keys, but we call it
    // one level at a time so we can report exactly which component fails.
    LONG r = RegCreateKeyExW(root, path, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, access,
                              nullptr, out, nullptr);
    return r;
}

// Check whether the current process token has the Administrators group
// and is in an elevated state (i.e. not filtered by UAC).
static bool IsElevated()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION elev = {};
    DWORD sz = sizeof(elev);
    bool elevated = false;
    if (GetTokenInformation(tok, TokenElevation, &elev, sz, &sz))
        elevated = (elev.TokenIsElevated != 0);
    CloseHandle(tok);
    return elevated;
}

// -
// COM server registration
// -
static bool RegisterCOMServer(const std::wstring& dllPath, const GUID& clsid)
{
    std::wstring clsidStr = GuidToString(clsid);
    std::wstring keyPath  = std::wstring(kCOMServerBase) + L"\\" + clsidStr;

    HKEY hk = nullptr;
    LONG r = CreateKeyPath(HKEY_LOCAL_MACHINE, keyPath.c_str(),
                            KEY_WRITE, &hk);
    if (r != ERROR_SUCCESS) {
        DWORD err = (DWORD)r;
        wprintf(L"  [FAIL] Cannot create CLSID key.\n"
                L"         Path : HKLM\\%s\n"
                L"         Error: %lu  %s\n",
                keyPath.c_str(), err, Win32Msg(err).c_str());
        return false;
    }
    const wchar_t* name = L"OpenAL Soft Spatial Audio Renderer";
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (const BYTE*)name, (DWORD)((wcslen(name)+1)*sizeof(wchar_t)));
    RegCloseKey(hk);

    std::wstring inprocPath = keyPath + L"\\InProcServer32";
    r = CreateKeyPath(HKEY_LOCAL_MACHINE, inprocPath.c_str(),
                       KEY_WRITE, &hk);
    if (r != ERROR_SUCCESS) {
        DWORD err = (DWORD)r;
        wprintf(L"  [FAIL] Cannot create InProcServer32 key.\n"
                L"         Path : HKLM\\%s\n"
                L"         Error: %lu  %s\n",
                inprocPath.c_str(), err, Win32Msg(err).c_str());
        return false;
    }
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (const BYTE*)dllPath.c_str(),
        (DWORD)((dllPath.size()+1)*sizeof(wchar_t)));
    const wchar_t* model = L"Both";
    RegSetValueExW(hk, L"ThreadingModel", 0, REG_SZ,
        (const BYTE*)model, (DWORD)((wcslen(model)+1)*sizeof(wchar_t)));
    RegCloseKey(hk);

    wprintf(L"  [OK]  COM server registered.\n"
            L"        CLSID : %s\n"
            L"        DLL   : %s\n",
            clsidStr.c_str(), dllPath.c_str());
    return true;
}

// -
// Spatial Sound provider registration
// -
static bool RegisterSpatialProvider(const GUID& clsid,
                                     const std::wstring& dllPath)
{
    std::wstring clsidStr = GuidToString(clsid);
    std::wstring keyPath  = std::wstring(kSpatialKeyBase) + L"\\" + clsidStr;

    // Create intermediate key first so we get a precise error if it fails
    HKEY hBase = nullptr;
    LONG r = CreateKeyPath(HKEY_LOCAL_MACHINE, kSpatialKeyBase,
                            KEY_WRITE, &hBase);
    if (r != ERROR_SUCCESS) {
        DWORD err = (DWORD)r;
        wprintf(L"  [FAIL] Cannot create base spatial key.\n"
                L"         Path : HKLM\\%s\n"
                L"         Error: %lu  %s\n\n"
                L"  TIP: If error is 5 (Access Denied), try:\n"
                L"    psexec -s -i RegisterProvider.exe register \"<dll>\"\n"
                L"  to run as SYSTEM, or take ownership in regedit first.\n",
                kSpatialKeyBase, err, Win32Msg(err).c_str());
        RegCloseKey(hBase);
        return false;
    }
    RegCloseKey(hBase);

    HKEY hk = nullptr;
    r = CreateKeyPath(HKEY_LOCAL_MACHINE, keyPath.c_str(), KEY_WRITE, &hk);
    if (r != ERROR_SUCCESS) {
        DWORD err = (DWORD)r;
        wprintf(L"  [FAIL] Cannot create provider subkey.\n"
                L"         Path : HKLM\\%s\n"
                L"         Error: %lu  %s\n",
                keyPath.c_str(), err, Win32Msg(err).c_str());
        return false;
    }

    const wchar_t* dispName = L"OpenAL Soft 3D Audio (HRTF)";
    RegSetValueExW(hk, L"DisplayName", 0, REG_SZ,
        (const BYTE*)dispName,
        (DWORD)((wcslen(dispName)+1)*sizeof(wchar_t)));

    std::wstring iconPath = dllPath + L",0";
    RegSetValueExW(hk, L"IconPath", 0, REG_SZ,
        (const BYTE*)iconPath.c_str(),
        (DWORD)((iconPath.size()+1)*sizeof(wchar_t)));

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
    wprintf(L"  [OK]  Spatial provider key written.\n"
            L"        Path: HKLM\\%s\n", keyPath.c_str());
    return true;
}

// -
// Unregister
// -
static bool UnregisterProvider(const GUID& clsid)
{
    std::wstring clsidStr = GuidToString(clsid);
    bool ok = true;

    std::wstring provKey = std::wstring(kSpatialKeyBase) + L"\\" + clsidStr;
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, provKey.c_str());
    if (r == ERROR_SUCCESS)
        wprintf(L"  [OK]  Deleted spatial key: HKLM\\%s\n", provKey.c_str());
    else if (r == ERROR_FILE_NOT_FOUND)
        wprintf(L"  [--]  Spatial key not found (already removed).\n");
    else {
        wprintf(L"  [FAIL] Could not delete spatial key. Error: %lu  %s\n",
                (DWORD)r, Win32Msg((DWORD)r).c_str());
        ok = false;
    }

    std::wstring comKey = std::wstring(kCOMServerBase) + L"\\" + clsidStr;
    r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, comKey.c_str());
    if (r == ERROR_SUCCESS)
        wprintf(L"  [OK]  Deleted COM key: HKLM\\%s\n", comKey.c_str());
    else if (r == ERROR_FILE_NOT_FOUND)
        wprintf(L"  [--]  COM key not found (already removed).\n");
    else {
        wprintf(L"  [FAIL] Could not delete COM key. Error: %lu  %s\n",
                (DWORD)r, Win32Msg((DWORD)r).c_str());
        ok = false;
    }

    return ok;
}

// -
// List registered providers under our key
// -
static void ListProviders()
{
    HKEY hk = nullptr;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSpatialKeyBase,
                            0, KEY_READ, &hk);
    if (r != ERROR_SUCCESS) {
        wprintf(L"  No providers found under HKLM\\%s\n"
                L"  (key does not exist yet, or no read access)\n",
                kSpatialKeyBase);
        return;
    }
    wprintf(L"  Providers under HKLM\\%s:\n", kSpatialKeyBase);
    wchar_t sub[256]; DWORD idx = 0;
    while (RegEnumKeyW(hk, idx++, sub, 256) == ERROR_SUCCESS) {
        wprintf(L"  %s", sub);
        HKEY hSub = nullptr;
        std::wstring subPath = std::wstring(kSpatialKeyBase) + L"\\" + sub;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subPath.c_str(),
                0, KEY_READ, &hSub) == ERROR_SUCCESS) {
            wchar_t name[256] = {}; DWORD sz = sizeof(name);
            if (RegQueryValueExW(hSub, L"DisplayName", nullptr,
                    nullptr, (BYTE*)name, &sz) == ERROR_SUCCESS)
                wprintf(L"  ->  %s", name);
            RegCloseKey(hSub);
        }
        wprintf(L"\n");
    }
    RegCloseKey(hk);
}

// -
// Diagnose: show access status for the key paths that matter
// -
static void Diagnose()
{
    wprintf(L"\n  Elevation status   : %s\n",
        IsElevated() ? L"Elevated (OK)" : L"NOT elevated - relaunch as Administrator");

    struct { const wchar_t* label; const wchar_t* path; REGSAM access; } tests[] = {
        { L"COM CLSID (write) ", kCOMServerBase,   KEY_WRITE },
        { L"SpatialRendering  ", kSpatialKeyBase,  KEY_WRITE },
        { L"MMDevices (write) ", kMMDevicesBase,   KEY_WRITE },
        { L"MMDevices (read)  ", kMMDevicesBase,   KEY_READ  },
    };

    for (auto& t : tests) {
        HKEY hk = nullptr;
        LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, t.path,
                                0, t.access, &hk);
        if (r == ERROR_SUCCESS) {
            wprintf(L"  %-20s: OK       HKLM\\%s\n", t.label, t.path);
            RegCloseKey(hk);
        } else {
            wprintf(L"  %-20s: Error %lu (%s)  HKLM\\%s\n",
                    t.label, (DWORD)r, Win32Msg((DWORD)r).c_str(), t.path);
        }
    }

    wprintf(L"\n  If MMDevices write fails with error 5 (Access Denied),\n"
            L"  run the tool via PsExec as SYSTEM:\n"
            L"    psexec -s -i RegisterProvider.exe register \"<dll>\"\n"
            L"  or manually create the key in regedit after taking ownership.\n\n");
}

// -
// Entry point
// -
int wmain(int argc, wchar_t* argv[])
{
    // Fix console output encoding so ASCII-only strings render correctly
    // on both cmd.exe and PowerShell regardless of system locale.
    SetConsoleOutputCP(CP_UTF8);

    wprintf(L"OpenAL Spatial Audio - Windows Spatial Sound Provider Registration\n"
            L"===================================================================\n\n");

    if (!IsElevated()) {
        wprintf(L"WARNING: Process is not elevated. Registry writes to HKLM will\n"
                L"likely fail with Access Denied (error 5).\n"
                L"Right-click the terminal and choose 'Run as Administrator'.\n\n");
    }

    if (argc < 2) {
        wprintf(L"Usage:\n"
                L"  RegisterProvider.exe  register   [path\\to\\openal_spatial.dll]\n"
                L"  RegisterProvider.exe  unregister\n"
                L"  RegisterProvider.exe  list\n"
                L"  RegisterProvider.exe  diagnose\n\n"
                L"Run as Administrator.\n");
        return 1;
    }

    std::wstring cmd = argv[1];

    if (cmd == L"diagnose") {
        Diagnose();
        return 0;
    }

    if (cmd == L"list") {
        ListProviders();
        return 0;
    }

    if (cmd == L"unregister") {
        wprintf(L"[Unregister]\n");
        bool ok = UnregisterProvider(CLSID_OpenALSpatialProvider);
        wprintf(ok ? L"\nDone.\n" : L"\nCompleted with errors (see above).\n");
        return ok ? 0 : 1;
    }

    if (cmd == L"register") {
        std::wstring dllPath;
        if (argc >= 3) {
            dllPath = argv[2];
        } else {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            dllPath = exePath;
            size_t slash = dllPath.rfind(L'\\');
            if (slash != std::wstring::npos) dllPath.resize(slash + 1);
            dllPath += L"openal_spatial.dll";
        }

        if (!PathFileExistsW(dllPath.c_str())) {
            wprintf(L"  [FAIL] DLL not found: %s\n"
                    L"  Pass the correct path as the second argument.\n",
                    dllPath.c_str());
            return 1;
        }
        wprintf(L"  DLL path: %s\n\n", dllPath.c_str());

        wprintf(L"[Step 1] Registering COM server...\n");
        if (!RegisterCOMServer(dllPath, CLSID_OpenALSpatialProvider)) {
            wprintf(L"\nRun 'RegisterProvider.exe diagnose' for access details.\n");
            return 1;
        }

        wprintf(L"\n[Step 2] Registering spatial audio provider...\n");
        if (!RegisterSpatialProvider(CLSID_OpenALSpatialProvider, dllPath)) {
            wprintf(L"\nRun 'RegisterProvider.exe diagnose' for access details.\n");
            return 1;
        }

        wprintf(L"\nRegistration complete!\n"
                L"\nTo activate in an application, call:\n"
                L"  ActivateSpatialAudioStream() with the CLSID above.\n\n"
                L"To verify the keys were written:\n"
                L"  RegisterProvider.exe list\n\n"
                L"Note: A restart of the Windows Audio service may be needed:\n"
                L"  net stop audiosrv && net start audiosrv\n"
                L"  (run in an elevated prompt)\n");
        return 0;
    }

    wprintf(L"Unknown command: %s\n", cmd.c_str());
    return 1;
}
