// OpenWebCamSetup: installs/removes the OpenWebCam virtual camera.
//
//   OpenWebCamSetup install    copy OpenWebCam.dll (next to this exe) to Program Files,
//                              register its COM class, create the log folder, and
//                              register a system-wide virtual camera (persists across reboots)
//   OpenWebCamSetup uninstall  remove the virtual camera and COM registration
//   OpenWebCamSetup prefer-wifi  keep internet traffic off the phone's USB tethering
//                              (install does this too; rerun after using a new USB port)
//
// Runs elevated (see setup.manifest). Nothing stays running afterwards.

#include "../common/net.h"
#include "../common/openwebcam.h"

#include <windows.h>
#include <aclapi.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <sddl.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

static const wchar_t kClsidKey[] = L"SOFTWARE\\Classes\\CLSID\\" OWC_CLSID_STRING;

static std::wstring Expand(const wchar_t* s) {
    wchar_t buf[MAX_PATH];
    ExpandEnvironmentStringsW(s, buf, MAX_PATH);
    return buf;
}

static bool Fail(const char* what, DWORD err) {
    fprintf(stderr, "%s failed: %lu (0x%08lX)\n", what, err, err);
    return false;
}

// A loaded DLL can't be overwritten but can be renamed; the old copy is deleted at reboot.
static bool CopyDll(const std::wstring& src, const std::wstring& dst) {
    if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring old = dst + L".old" + std::to_wstring(GetTickCount64());
        if (!MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return Fail("Rename old DLL", GetLastError());
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return Fail("Copy DLL", GetLastError());
    return true;
}

// Log folder: Frame Server (LocalService, possibly write-restricted) must write; users may read/delete.
static bool CreateDataDir() {
    std::wstring dir = Expand(OWC_DATA_DIR);
    CreateDirectoryW(dir.c_str(), nullptr);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;LS)(A;OICI;0x1301bf;;;WR)(A;OICI;0x1301bf;;;BU)",
            SDDL_REVISION_1, &sd, nullptr))
        return Fail("Build log folder ACL", GetLastError());
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    DWORD err = SetNamedSecurityInfoW(&dir[0], SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                      nullptr, dacl, nullptr);
    LocalFree(sd);
    if (err != ERROR_SUCCESS) return Fail("Set log folder ACL", err);
    wprintf(L"Log file: %ls\\log.txt\n", dir.c_str());
    return true;
}

static bool SetRegString(HKEY key, const wchar_t* name, const std::wstring& value) {
    LSTATUS rc = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    return rc == ERROR_SUCCESS || Fail("Write registry", rc);
}

static bool RegisterCom(const std::wstring& dllPath) {
    HKEY clsid = nullptr, inproc = nullptr;
    LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kClsidKey, 0, nullptr, 0, KEY_WRITE, nullptr, &clsid, nullptr);
    if (rc != ERROR_SUCCESS) return Fail("Create CLSID key", rc);
    rc = RegCreateKeyExW(clsid, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr);
    bool ok = rc == ERROR_SUCCESS ? SetRegString(clsid, nullptr, OWC_FRIENDLY_NAME) &&
                                        SetRegString(inproc, nullptr, dllPath) &&
                                        SetRegString(inproc, L"ThreadingModel", L"Both")
                                  : Fail("Create InprocServer32 key", rc);
    if (inproc) RegCloseKey(inproc);
    RegCloseKey(clsid);
    return ok;
}

// USB tethering adds a default route through the phone that Windows ranks above
// Wi-Fi, so all internet traffic would go over the phone. A high metric keeps
// the phone link for OpenWebCam only (applied now and saved for next time).
// prefer=false restores automatic metrics.
static int SetTetherMetric(bool prefer) {
    const DWORD kMetric = 500;
    int count = 0;
    owc::ForEachAdapter(AF_INET, 0, [&](const IP_ADAPTER_ADDRESSES* a) {
        if (!owc::IsTetherAdapter(a)) return;
        MIB_IPINTERFACE_ROW row;
        InitializeIpInterfaceEntry(&row);
        row.Family = AF_INET;
        row.InterfaceLuid = a->Luid;
        if (GetIpInterfaceEntry(&row) == NO_ERROR) {
            row.SitePrefixLength = 0;  // required for IPv4 by SetIpInterfaceEntry
            row.UseAutomaticMetric = !prefer;
            if (prefer) row.Metric = kMetric;
            SetIpInterfaceEntry(&row);
        }
        std::string guid = a->AdapterName;
        std::wstring key = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" +
                           std::wstring(guid.begin(), guid.end());
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            if (prefer) RegSetValueExW(k, L"InterfaceMetric", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&kMetric), sizeof kMetric);
            else RegDeleteValueW(k, L"InterfaceMetric");
            RegCloseKey(k);
        }
        wprintf(L"%ls: %ls\n", a->FriendlyName, prefer ? L"internet traffic will prefer other networks" : L"automatic metric restored");
        ++count;
    });
    if (!count && prefer)
        printf("No USB tethering adapter connected. Turn tethering on and run \"OpenWebCamSetup prefer-wifi\".\n");
    return 0;
}

static HRESULT CreateCamera(ComPtr<IMFVirtualCamera>& cam) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                 MFVirtualCameraAccess_AllUsers, OWC_FRIENDLY_NAME, OWC_CLSID_STRING, nullptr, 0,
                                 &cam);
}

static int Install() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring src = exe;
    src = src.substr(0, src.find_last_of(L'\\') + 1) + L"OpenWebCam.dll";
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"Missing %ls\n", src.c_str());
        return 1;
    }
    std::wstring dir = Expand(L"%ProgramFiles%\\OpenWebCam");
    std::wstring dst = dir + L"\\OpenWebCam.dll";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!CopyDll(src, dst) || !CreateDataDir() || !RegisterCom(dst)) return 1;
    wprintf(L"Installed %ls\n", dst.c_str());

    ComPtr<IMFVirtualCamera> cam;
    HRESULT hr = CreateCamera(cam);
    if (SUCCEEDED(hr)) hr = cam->Start(nullptr);
    if (FAILED(hr)) {
        Fail("Register virtual camera", hr);
        return 1;
    }
    printf("Virtual camera \"OpenWebCam\" registered. Open the Camera app to test.\n");
    SetTetherMetric(true);
    return 0;
}

static int Uninstall() {
    ComPtr<IMFVirtualCamera> cam;
    HRESULT hr = CreateCamera(cam);
    if (SUCCEEDED(hr)) hr = cam->Remove();
    if (FAILED(hr)) Fail("Remove virtual camera", hr);
    else printf("Virtual camera removed.\n");

    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    SetTetherMetric(false);
    std::wstring dll = Expand(L"%ProgramFiles%\\OpenWebCam\\OpenWebCam.dll");
    if (!DeleteFileW(dll.c_str())) MoveFileExW(dll.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    printf("COM registration removed. Logs kept in %%ProgramData%%\\OpenWebCam.\n");
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"prefer-wifi") return SetTetherMetric(true);
    if (cmd != L"install" && cmd != L"uninstall") {
        printf("Usage: OpenWebCamSetup install | uninstall | prefer-wifi\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        printf("Media Foundation unavailable (Windows N edition without the Media Feature Pack?)\n");
        return 1;
    }
    int rc = cmd == L"install" ? Install() : Uninstall();
    MFShutdown();
    CoUninitialize();
    return rc;
}
