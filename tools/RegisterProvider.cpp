/**
 * RegisterProvider.cpp
 * ============================================================
 * Registers / unregisters OpenALSpatialAudio as a Windows
 * Spatial Sound provider, allowing apps (and Windows itself)
 * to select it through the normal "Sound" control panel or
 * via ISpatialAudioClient::GetSupportedAudioObjectFormatEnumerator.
 *
 * This is a standalone console tool – run as Administrator.
 *
 * Usage:
 *   RegisterProvider.exe  register   [path-to-openal_spatial.dll]
 *   RegisterProvider.exe  unregister
 *   RegisterProvider.exe  list
 *
 * What this does:
 *   1. Registers the DLL as a COM in-process server.
 *   2. Writes the required registry keys under
 *      HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\
 *             MMDevices\SpatialAudioEndpoint\<CLSID>
 *      that Windows Audio Session API (WASAPI) reads to
 *      enumerate spatial sound providers.
 *
 * WARNING: Modifying the spatial audio provider list requires
 * Administrator rights and can affect system audio. Always
 * keep a backup of the original Windows Sonic provider CLSID.
 * ============================================================
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

// ─────────────────────────────────────────────────────────────
// Our provider CLSID
// {9A3B4C5D-6E7F-8901-ABCD-EF1234567890}
// Generate a fresh GUID with uuidgen.exe for production use.
// ─────────────────────────────────────────────────────────────
// {9A3B4C5D-6E7F-8901-ABCD-EF1234567890}
DEFINE_GUID(CLSID_OpenALSpatialProvider,
    0x9a3b4c5d, 0x6e7f, 0x8901,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

// Windows Sonic for Headphones CLSID (built-in)
// {B2B88BE7-07A8-4BC7-B25B-6A68B9B0EE93}
DEFINE_GUID(CLSID_WindowsSonic,
    0xb2b88be7, 0x07a8, 0x4bc7,
    0xb2, 0x5b, 0x6a, 0x68, 0xb9, 0xb0, 0xee, 0x93);

static const wchar_t* kProviderKeyBase =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
    L"MMDevices\\SpatialAudioEndpoint";

static const wchar_t* kCOMServerBase =
    L"SOFTWARE\\Classes\\CLSID";

// ─────────────────────────────────────────────────────────────
// Utility: GUID <-> string conversions
// ─────────────────────────────────────────────────────────────
std::wstring GuidToString(const GUID& g)
{
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return buf;
}

// ─────────────────────────────────────────────────────────────
// COM server registration
// ─────────────────────────────────────────────────────────────
bool RegisterCOMServer(const std::wstring& dllPath, const GUID& clsid)
{
    std::wstring clsidStr = GuidToString(clsid);
    std::wstring keyPath  = std::wstring(kCOMServerBase) + L"\\" + clsidStr;

    // Create CLSID key with friendly name
    HKEY hk;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(),
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) {
        std::wcerr << L"Failed to create CLSID key\n";
        return false;
    }
    const wchar_t* name = L"OpenAL Soft Spatial Audio Renderer";
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (BYTE*)name, (DWORD)((wcslen(name) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);

    // InProcServer32
    std::wstring inprocPath = keyPath + L"\\InProcServer32";
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, inprocPath.c_str(),
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) {
        std::wcerr << L"Failed to create InProcServer32 key\n";
        return false;
    }
    RegSetValueExW(hk, nullptr, 0, REG_SZ,
        (BYTE*)dllPath.c_str(),
        (DWORD)((dllPath.size() + 1) * sizeof(wchar_t)));
    const wchar_t* model = L"Both";
    RegSetValueExW(hk, L"ThreadingModel", 0, REG_SZ,
        (BYTE*)model, (DWORD)((wcslen(model) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);

    std::wcout << L"COM server registered\n  CLSID: " << clsidStr << L"\n"
               << L"  DLL:   " << dllPath << L"\n";
    return true;
}

// ─────────────────────────────────────────────────────────────
// Spatial Sound provider registration
// Writes under HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\
//              MMDevices\SpatialAudioEndpoint\<CLSID>
// ─────────────────────────────────────────────────────────────
bool RegisterSpatialProvider(const GUID& clsid,
                              const std::wstring& dllPath)
{
    std::wstring clsidStr = GuidToString(clsid);
    std::wstring keyPath  = std::wstring(kProviderKeyBase)
                          + L"\\" + clsidStr;

    HKEY hk;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(),
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) {
        std::wcerr << L"Failed to create provider key (need Administrator?)\n";
        return false;
    }

    // Friendly display name (shown in Sound control panel)
    const wchar_t* dispName = L"OpenAL Soft 3D Audio (HRTF)";
    RegSetValueExW(hk, L"DisplayName", 0, REG_SZ,
        (BYTE*)dispName, (DWORD)((wcslen(dispName)+1)*sizeof(wchar_t)));

    // Icon path (optional; point to the DLL's resource if it has one)
    std::wstring iconPath = dllPath + L",0";
    RegSetValueExW(hk, L"IconPath", 0, REG_SZ,
        (BYTE*)iconPath.c_str(),
        (DWORD)((iconPath.size()+1)*sizeof(wchar_t)));

    // Supported object types bitfield
    // Set to the full Dolby Atmos / Windows Sonic bed + dynamic objects
    DWORD objTypes = 0x1FFFF;  // All bed channels + dynamic
    RegSetValueExW(hk, L"StaticObjectTypeMask", 0, REG_DWORD,
        (BYTE*)&objTypes, sizeof(DWORD));

    // MaxDynamicObjects
    DWORD maxDyn = 256;
    RegSetValueExW(hk, L"MaxDynamicObjectCount", 0, REG_DWORD,
        (BYTE*)&maxDyn, sizeof(DWORD));

    // Flags: 0 = compatible with headphones
    DWORD flags = 0;
    RegSetValueExW(hk, L"Flags", 0, REG_DWORD,
        (BYTE*)&flags, sizeof(DWORD));

    RegCloseKey(hk);
    std::wcout << L"Spatial provider registered:\n"
               << L"  Key: " << keyPath << L"\n";
    return true;
}

// ─────────────────────────────────────────────────────────────
// Unregister
// ─────────────────────────────────────────────────────────────
bool UnregisterProvider(const GUID& clsid)
{
    std::wstring clsidStr = GuidToString(clsid);

    // Remove spatial provider key
    std::wstring provKey = std::wstring(kProviderKeyBase) + L"\\" + clsidStr;
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, provKey.c_str());
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
        std::wcerr << L"Failed to delete provider key\n";
    }

    // Remove COM registration
    std::wstring comKey = std::wstring(kCOMServerBase) + L"\\" + clsidStr;
    r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, comKey.c_str());
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
        std::wcerr << L"Failed to delete COM key\n";
    }

    std::wcout << L"Unregistered: " << clsidStr << L"\n";
    return true;
}

// ─────────────────────────────────────────────────────────────
// List registered providers
// ─────────────────────────────────────────────────────────────
void ListProviders()
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kProviderKeyBase,
            0, KEY_READ, &hk) != ERROR_SUCCESS) {
        std::wcout << L"No spatial audio providers found.\n";
        return;
    }

    wchar_t subKey[256];
    DWORD index = 0;
    std::wcout << L"Registered Spatial Sound providers:\n";
    while (RegEnumKeyW(hk, index++, subKey, 256) == ERROR_SUCCESS) {
        std::wcout << L"  " << subKey;

        // Read display name
        HKEY hSub;
        std::wstring subPath = std::wstring(kProviderKeyBase)
                             + L"\\" + subKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subPath.c_str(),
                0, KEY_READ, &hSub) == ERROR_SUCCESS) {
            wchar_t name[256] = {};
            DWORD sz = sizeof(name);
            if (RegQueryValueExW(hSub, L"DisplayName", nullptr,
                    nullptr, (BYTE*)name, &sz) == ERROR_SUCCESS) {
                std::wcout << L"  →  " << name;
            }
            RegCloseKey(hSub);
        }
        std::wcout << L"\n";
    }
    RegCloseKey(hk);
}

// ─────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────
int wmain(int argc, wchar_t* argv[])
{
    std::wcout <<
        L"OpenAL Spatial Audio – Windows Spatial Sound Provider Registration\n"
        L"===================================================================\n\n";

    if (argc < 2) {
        std::wcout <<
            L"Usage:\n"
            L"  RegisterProvider.exe  register   [path\\to\\openal_spatial.dll]\n"
            L"  RegisterProvider.exe  unregister\n"
            L"  RegisterProvider.exe  list\n\n"
            L"Run as Administrator.\n";
        return 1;
    }

    std::wstring cmd = argv[1];

    if (cmd == L"list") {
        ListProviders();
        return 0;
    }

    if (cmd == L"unregister") {
        if (!UnregisterProvider(CLSID_OpenALSpatialProvider)) return 1;
        std::wcout << L"\nProvider unregistered. "
                      L"Windows will fall back to Windows Sonic.\n";
        return 0;
    }

    if (cmd == L"register") {
        std::wstring dllPath;
        if (argc >= 3) {
            dllPath = argv[2];
        } else {
            // Default: same directory as this executable
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            dllPath = exePath;
            size_t lastSlash = dllPath.rfind(L'\\');
            if (lastSlash != std::wstring::npos)
                dllPath.resize(lastSlash + 1);
            dllPath += L"openal_spatial.dll";
        }

        // Verify the DLL exists
        if (!PathFileExistsW(dllPath.c_str())) {
            std::wcerr << L"DLL not found: " << dllPath << L"\n"
                       << L"Specify the correct path as the second argument.\n";
            return 1;
        }

        if (!RegisterCOMServer(dllPath, CLSID_OpenALSpatialProvider)) return 1;
        if (!RegisterSpatialProvider(CLSID_OpenALSpatialProvider, dllPath)) return 1;

        std::wcout <<
            L"\nRegistration complete!\n"
            L"To activate:\n"
            L"  1. Open Sound settings → App volume and device preferences, OR\n"
            L"  2. Right-click the speaker tray icon → Spatial sound\n"
            L"     and select 'OpenAL Soft 3D Audio (HRTF)'\n"
            L"  3. Or call ActivateSpatialAudioObject() directly in your app.\n\n"
            L"Note: A reboot or audio service restart may be required:\n"
            L"  net stop audiosrv && net start audiosrv\n";
        return 0;
    }

    std::wcerr << L"Unknown command: " << cmd << L"\n";
    return 1;
}
