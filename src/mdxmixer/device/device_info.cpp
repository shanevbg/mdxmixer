#include "device_info.h"
#include "device/device_identity.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <objbase.h>

#pragma comment(lib, "cfgmgr32.lib")

namespace mdxm {

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// Spelled out rather than linked: devpkey.h declares these extern and the
// definitions live in a library the test build does not link.
const DEVPROPKEY kDevPkeyContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const DEVPROPKEY kDevPkeyBluetoothBattery = {
    {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};
// PKEY_DeviceInterface_Bluetooth_LastConnectedTime.
const DEVPROPKEY kDevPkeyBluetoothLastConnected = {
    {0x2bd67d8b, 0x8beb, 0x48d5, {0x87, 0xe0, 0x6c, 0xda, 0x34, 0x28, 0x04, 0x0a}}, 11};
// PKEY_DeviceInterface_Bluetooth_DeviceAddress -- the same property set as
// LastConnectedTime, index 1. A string of hex digits: the headset's own MAC.
//
// This is the only identifier on a Bluetooth audio device that survives a
// change of ADAPTER. The endpoint id does not, and neither does ContainerId:
// both are minted per pairing, so plugging in a different dongle re-pairs
// everything and every alias stops matching at once. Shane has been through
// several adapters and has eighteen pairings to show for it.
const DEVPROPKEY kDevPkeyBluetoothAddress = {
    {0x2bd67d8b, 0x8beb, 0x48d5, {0x87, 0xe0, 0x6c, 0xda, 0x34, 0x28, 0x04, 0x0a}}, 1};
const DEVPROPKEY kDevPkeyDeviceIsPresent = {
    {0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 5};

std::wstring GuidToString(const GUID& g) {
    wchar_t buf[64];
    swprintf(buf, 64, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
             g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

std::wstring ReadContainer(DEVINST inst) {
    DEVPROPTYPE type = 0;
    GUID guid = {};
    ULONG size = sizeof(guid);
    if (CM_Get_DevNode_PropertyW(inst, &kDevPkeyContainerId, &type, (PBYTE)&guid, &size, 0)
            != CR_SUCCESS || type != DEVPROP_TYPE_GUID)
        return {};
    return GuidToString(guid);
}

// The battery property is a single byte. Reading it as anything wider silently
// fails and reports "no device has a battery" on a machine where several do.
int ReadBatteryPercent(DEVINST inst) {
    DEVPROPTYPE type = 0;
    BYTE value = 0;
    ULONG size = sizeof(value);
    if (CM_Get_DevNode_PropertyW(inst, &kDevPkeyBluetoothBattery, &type, &value, &size, 0)
            != CR_SUCCESS || type != DEVPROP_TYPE_BYTE)
        return -1;
    return (int)value;
}

uint64_t ReadLastConnectedRaw(DEVINST inst) {
    DEVPROPTYPE type = 0;
    FILETIME ft = {};
    ULONG size = sizeof(ft);
    if (CM_Get_DevNode_PropertyW(inst, &kDevPkeyBluetoothLastConnected, &type, (PBYTE)&ft, &size, 0)
            != CR_SUCCESS || type != DEVPROP_TYPE_FILETIME)
        return 0;
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

std::wstring ReadBluetoothAddress(DEVINST inst) {
    DEVPROPTYPE type = 0;
    wchar_t buf[64] = {};
    ULONG size = sizeof(buf);
    if (CM_Get_DevNode_PropertyW(inst, &kDevPkeyBluetoothAddress, &type, (PBYTE)buf, &size, 0)
            != CR_SUCCESS || type != DEVPROP_TYPE_STRING)
        return {};
    // Lower-cased so two spellings of the same address cannot key two aliases.
    std::wstring out(buf);
    for (wchar_t& c : out) c = (wchar_t)towlower(c);
    return out;
}

bool ReadIsPresent(DEVINST inst) {
    DEVPROPTYPE type = 0;
    DEVPROP_BOOLEAN value = DEVPROP_FALSE;
    ULONG size = sizeof(value);
    if (CM_Get_DevNode_PropertyW(inst, &kDevPkeyDeviceIsPresent, &type, (PBYTE)&value, &size, 0)
            != CR_SUCCESS || type != DEVPROP_TYPE_BOOLEAN)
        return false;
    return value != DEVPROP_FALSE;
}

} // namespace

std::vector<BluetoothInfo> ReadBluetoothInfo() {
    std::vector<BluetoothInfo> out;
    try {
        ULONG len = 0;
        if (CM_Get_Device_ID_List_SizeW(&len, L"BTHENUM", CM_GETIDLIST_FILTER_ENUMERATOR)
                != CR_SUCCESS || len < 2)
            return out;
        std::vector<wchar_t> buf(len);
        if (CM_Get_Device_ID_ListW(L"BTHENUM", buf.data(), len,
                                   CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS)
            return out;

        for (const wchar_t* id = buf.data(); *id; id += wcslen(id) + 1) {
            DEVINST inst = 0;
            if (CM_Locate_DevNodeW(&inst, (DEVINSTID_W)id, CM_LOCATE_DEVNODE_PHANTOM)
                    != CR_SUCCESS)
                continue;
            BluetoothInfo info;
            info.containerId = ReadContainer(inst);
            if (info.containerId.empty()) continue;
            info.present = ReadIsPresent(inst);
            // Battery only from an ATTACHED node: a phantom's reading is
            // whatever it held on the way out.
            if (info.present) {
                int pct = ReadBatteryPercent(inst);
                if (pct >= 0) info.battery = pct;
            }
            info.lastConnectedRaw = ReadLastConnectedRaw(inst);
            info.btAddress = ReadBluetoothAddress(inst);

            // Several nodes share one container ("<name>", "<name> Hands-Free
            // AG", "<name> Avrcp Transport"). Merge them: take any battery
            // that reports, the NEWEST last-connected, and present if any node
            // is attached.
            auto it = out.end();
            for (auto i = out.begin(); i != out.end(); ++i)
                if (i->containerId == info.containerId) { it = i; break; }
            if (it == out.end()) {
                out.push_back(info);
            } else {
                if (info.battery >= 0) it->battery = info.battery;
                if (info.lastConnectedRaw > it->lastConnectedRaw)
                    it->lastConnectedRaw = info.lastConnectedRaw;
                it->present = it->present || info.present;
                // Any node that carries it: the address is a property of the
                // headset, so the sibling nodes of one container either agree
                // or say nothing.
                if (it->btAddress.empty()) it->btAddress = info.btAddress;
            }
        }
    } catch (...) {}
    return out;
}

uint64_t EndpointLastSeenUtc(const std::wstring& endpointId, bool isRender) {
    const size_t brace = endpointId.rfind(L'{');
    if (brace == std::wstring::npos) return 0;

    std::wstring path = L"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\\";
    path += isRender ? L"Render\\" : L"Capture\\";
    path += endpointId.substr(brace);

    HKEY key = nullptr;
    // WOW64_64KEY: a 32-bit build would otherwise be redirected to a view of
    // the registry that does not hold these.
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return 0;
    FILETIME written = {};
    const LONG r = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, &written);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return 0;
    return ((uint64_t)written.dwHighDateTime << 32) | written.dwLowDateTime;
}

std::wstring EndpointContainerId(const std::wstring& endpointId) {
    try {
        ComScope com;
        IMMDeviceEnumerator* enumr = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&enumr)) || !enumr)
            return {};
        IMMDevice* dev = nullptr;
        std::wstring out;
        if (SUCCEEDED(enumr->GetDevice(endpointId.c_str(), &dev)) && dev) {
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                // PKEY_Device_ContainerId has the same fmtid/pid as the PnP key.
                PROPERTYKEY key = { kDevPkeyContainerId.fmtid, kDevPkeyContainerId.pid };
                if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_CLSID && pv.puuid) {
                    out = GuidToString(*pv.puuid);
                    // The placeholder container is shared by everything that has
                    // none; handing it back would join unrelated devices.
                    if (IsNullContainer(out)) out.clear();
                }
                PropVariantClear(&pv);
                props->Release();
            }
            dev->Release();
        }
        enumr->Release();
        return out;
    } catch (...) { return {}; }
}

} // namespace mdxm
