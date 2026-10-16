#include "storage.hpp"
#include <comdef.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <set>

namespace diskboot {
static void hr_check(HRESULT hr, const char *context) {
    if (FAILED(hr))
        throw Error(std::string(context) + " (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")");
}
ComRuntime::ComRuntime() {
    hr_check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM");
    auto hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                   RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
        CoUninitialize();
        hr_check(hr, "Initialize WMI security");
    }
}
ComRuntime::~ComRuntime() {
    CoUninitialize();
}
Wmi::Wmi(const wchar_t *name_space) {
    ComPtr<IWbemLocator> locator;
    hr_check(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)),
             "Create WMI locator");
    hr_check(locator->ConnectServer(_bstr_t(name_space), nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT,
                                    nullptr, nullptr, &services_),
             "Connect WMI namespace");
    hr_check(CoSetProxyBlanket(services_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                               RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE),
             "Set WMI security");
}
std::vector<ComPtr<IWbemClassObject>> Wmi::query(const std::wstring &text) {
    ComPtr<IEnumWbemClassObject> enumeration;
    hr_check(services_->ExecQuery(_bstr_t(L"WQL"), _bstr_t(text.c_str()),
                                  WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumeration),
             "Query storage provider");
    std::vector<ComPtr<IWbemClassObject>> result;
    for (;;) {
        ComPtr<IWbemClassObject> object;
        ULONG                    count = 0;
        const auto               hr    = enumeration->Next(30000, 1, &object, &count);
        hr_check(hr, "Enumerate storage provider");
        require(hr != WBEM_S_TIMEDOUT, "Storage provider timed out");
        if (!count)
            break;
        require(result.size() < 4096, "Storage inventory exceeds supported limit");
        result.push_back(object);
    }
    return result;
}
static _variant_t get(IWbemClassObject *object, const wchar_t *key) {
    _variant_t value;
    hr_check(object->Get(key, 0, &value, nullptr, nullptr), "Read storage property");
    return value;
}
static std::string text_property(IWbemClassObject *object, const wchar_t *key) {
    auto value = get(object, key);
    if (value.vt == VT_NULL || value.vt == VT_EMPTY)
        return {};
    require(value.vt == VT_BSTR, "Unexpected storage string type");
    return utf8(std::wstring(value.bstrVal, SysStringLen(value.bstrVal)));
}
static Bytes number(IWbemClassObject *object, const wchar_t *key) {
    auto value = get(object, key);
    if (value.vt == VT_BSTR) {
        auto str = utf8(std::wstring(value.bstrVal));
        if (str == "0")
            return 0;
        return parse_size(str);
    }
    if (value.vt == VT_I4) {
        require(value.lVal >= 0, "Negative storage property");
        return value.lVal;
    }
    if (value.vt == VT_UI4)
        return value.ulVal;
    if (value.vt == VT_UI2)
        return value.uiVal;
    if (value.vt == VT_I2) {
        require(value.iVal >= 0, "Negative storage property");
        return value.iVal;
    }
    if (value.vt == VT_UI8)
        return value.ullVal;
    throw Error("Missing or invalid numeric storage property");
}
static bool boolean(IWbemClassObject *object, const wchar_t *key) {
    auto value = get(object, key);
    require(value.vt == VT_BOOL, "Unknown storage protection flag: " + utf8(key));
    return value.boolVal != VARIANT_FALSE;
}
ComPtr<IWbemClassObject> Wmi::call(IWbemClassObject *object, const wchar_t *method, const Json &args) {
    const auto klass = text_property(object, L"__CLASS"), path = text_property(object, L"__PATH");
    require(!klass.empty() && !path.empty(), "Unbound WMI method target");
    ComPtr<IWbemClassObject> definition, input_definition, input, output;
    hr_check(services_->GetObject(_bstr_t(wide(klass).c_str()), 0, nullptr, &definition, nullptr), "Read WMI class");
    hr_check(definition->GetMethod(method, 0, &input_definition, nullptr), "Read WMI method");
    if (input_definition) {
        hr_check(input_definition->SpawnInstance(0, &input), "Create WMI arguments");
        for (auto it = args.begin(); it != args.end(); ++it) {
            auto       key  = wide(it.key());
            CIMTYPE    type = 0;
            _variant_t old;
            hr_check(input->Get(key.c_str(), 0, &old, &type, nullptr), "Read WMI argument type");
            _variant_t value;
            if (it->is_boolean())
                value = _variant_t(it->get<bool>());
            else if (it->is_string())
                value = _variant_t(wide(it->get<std::string>()).c_str());
            else if (it->is_number_unsigned() && type == CIM_UINT64)
                value = _variant_t(std::to_wstring(it->get<Bytes>()).c_str());
            else if (it->is_number_integer() && it->get<std::int64_t>() >= 0 && it->get<Bytes>() <= MAXLONG)
                value = _variant_t(static_cast<long>(it->get<Bytes>()));
            else
                throw Error("Invalid WMI method argument");
            hr_check(input->Put(key.c_str(), 0, &value, 0), "Set WMI argument");
        }
    }
    hr_check(
        services_->ExecMethod(_bstr_t(wide(path).c_str()), _bstr_t(method), 0, nullptr, input.Get(), &output, nullptr),
        "Storage method call");
    require(output, "Storage method returned no result");
    const auto status = number(output.Get(), L"ReturnValue");
    require(status == 0,
            utf8(method) + " failed or requires reconciliation (provider status " + std::to_string(status) + ")");
    return output;
}
static std::string guid_string(const GUID &guid) {
    wchar_t buffer[40]{};
    require(StringFromGUID2(guid, buffer, 40) != 0, "Invalid GUID");
    auto result = lower(utf8(buffer));
    return result.substr(1, result.size() - 2);
}
static std::string normalized_guid(std::string guid) {
    guid = lower(std::move(guid));
    if (guid.starts_with('{') && guid.ends_with('}'))
        guid = guid.substr(1, guid.size() - 2);
    require(guid.size() == 36, "Invalid storage GUID");
    return guid;
}
static std::wstring assoc(const std::string &path, const wchar_t *klass) {
    return L"ASSOCIATORS OF {" + wide(path) + L"} WHERE AssocClass = " + klass;
}
Disk Storage::describe(IWbemClassObject *object) {
    Disk disk;
    disk.number         = static_cast<unsigned>(number(object, L"Number"));
    disk.wmi_path       = text_property(object, L"__PATH");
    auto &id            = disk.identity;
    id.guid             = normalized_guid(text_property(object, L"Guid"));
    id.unique_id        = text_property(object, L"UniqueId");
    id.serial           = text_property(object, L"SerialNumber");
    id.device_path      = lower(text_property(object, L"Path"));
    id.model            = text_property(object, L"FriendlyName");
    id.size             = number(object, L"Size");
    id.logical_sector   = static_cast<unsigned>(number(object, L"LogicalSectorSize"));
    id.physical_sector  = static_cast<unsigned>(number(object, L"PhysicalSectorSize"));
    id.bus              = static_cast<unsigned>(number(object, L"BusType"));
    disk.gpt            = number(object, L"PartitionStyle") == 2;
    disk.online         = !boolean(object, L"IsOffline");
    disk.writable       = !boolean(object, L"IsReadOnly");
    disk.healthy        = number(object, L"HealthStatus") == 0;
    disk.boot_chain     = boolean(object, L"IsBoot") || boolean(object, L"IsSystem");
    disk.protected_disk = disk.boot_chain;
    Handle handle(CreateFileW(wide(id.device_path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr));
    win_check(handle.value != INVALID_HANDLE_VALUE,
              "Open disk for layout verification (administrator may be required)");
    std::vector<unsigned char> buffer(4096);
    DWORD                      returned = 0;
    while (!DeviceIoControl(handle.value, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buffer.data(),
                            static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && buffer.size() < MiB, "Cannot read GPT layout");
        buffer.resize(buffer.size() * 2);
    }
    const auto *layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX *>(buffer.data());
    require(returned >= offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) && layout->PartitionCount <= 128 &&
                returned >= offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
                                layout->PartitionCount * sizeof(PARTITION_INFORMATION_EX),
            "Truncated GPT layout");
    require(layout->PartitionStyle == PARTITION_STYLE_GPT && guid_string(layout->Gpt.DiskId) == id.guid,
            "WMI and native disk identity disagree");
    require(layout->Gpt.StartingUsableOffset.QuadPart >= 0 && layout->Gpt.UsableLength.QuadPart > 0,
            "Invalid usable GPT bounds");
    disk.usable              = {static_cast<Bytes>(layout->Gpt.StartingUsableOffset.QuadPart),
                                static_cast<Bytes>(layout->Gpt.UsableLength.QuadPart)};
    auto        partitions   = wmi_.query(assoc(disk.wmi_path, L"MSFT_DiskToPartition"));
    std::size_t native_count = 0;
    for (unsigned i = 0; i < layout->PartitionCount; ++i)
        if (layout->PartitionEntry[i].PartitionLength.QuadPart)
            ++native_count;
    require(native_count == partitions.size(), "WMI and native partition counts disagree");
    for (const auto &obj : partitions) {
        Partition p;
        p.guid               = normalized_guid(text_property(obj.Get(), L"Guid"));
        p.type               = normalized_guid(text_property(obj.Get(), L"GptType"));
        p.wmi_path           = text_property(obj.Get(), L"__PATH");
        p.number             = static_cast<unsigned>(number(obj.Get(), L"PartitionNumber"));
        p.range              = {number(obj.Get(), L"Offset"), number(obj.Get(), L"Size")};
        p.boot               = boolean(obj.Get(), L"IsBoot");
        p.system             = boolean(obj.Get(), L"IsSystem");
        const auto read_only = get(obj.Get(), L"IsReadOnly");
        require(read_only.vt == VT_BOOL || read_only.vt == VT_NULL, "Invalid partition read-only flag");
        p.writable = disk.writable && !boolean(obj.Get(), L"IsOffline") &&
                     !(read_only.vt == VT_BOOL && read_only.boolVal != VARIANT_FALSE);
        require(number(obj.Get(), L"DiskNumber") == disk.number, "Partition belongs to another disk");
        bool matched = false;
        for (unsigned i = 0; i < layout->PartitionCount; ++i) {
            const auto &native = layout->PartitionEntry[i];
            if (native.PartitionLength.QuadPart && guid_string(native.Gpt.PartitionId) == p.guid) {
                require(!matched && native.PartitionNumber == p.number && native.StartingOffset.QuadPart >= 0 &&
                            p.range == Range{static_cast<Bytes>(native.StartingOffset.QuadPart),
                                             static_cast<Bytes>(native.PartitionLength.QuadPart)} &&
                            guid_string(native.Gpt.PartitionType) == p.type,
                        "WMI/native partition mismatch");
                p.attributes = native.Gpt.Attributes;
                matched      = true;
            }
        }
        require(matched, "Partition missing from native GPT");
        if (p.attributes & GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY)
            p.writable = false;
        if (p.boot || p.system || p.type == EspType || p.type == "de94bba4-06d1-4d40-a16a-bfd50179d6ac")
            disk.protected_disk = true;
        auto volumes = wmi_.query(assoc(p.wmi_path, L"MSFT_PartitionToVolume"));
        require(volumes.size() <= 1, "Partition has ambiguous volumes");
        if (!volumes.empty()) {
            auto v        = volumes.front();
            p.volume_path = text_property(v.Get(), L"Path");
            p.filesystem  = text_property(v.Get(), L"FileSystem");
            p.label       = text_property(v.Get(), L"FileSystemLabel");
            p.healthy     = number(v.Get(), L"HealthStatus") == 0;
            if (p.filesystem == "FAT32" || p.filesystem == "FAT")
                disk.protected_disk = true;
        }
        disk.partitions.push_back(p);
    }
    std::ranges::sort(disk.partitions, {}, [](const auto &p) { return p.range.offset; });
    return disk;
}
Inventory Storage::inventory(bool resize_bounds) {
    Inventory          result;
    std::set<unsigned> running_disks;
    try {
        wchar_t windows[MAX_PATH]{}, root[MAX_PATH]{}, volume[MAX_PATH]{};
        win_check(GetWindowsDirectoryW(windows, MAX_PATH) != 0, "Locate running Windows");
        win_check(GetVolumePathNameW(windows, root, MAX_PATH), "Locate Windows volume");
        win_check(GetVolumeNameForVolumeMountPointW(root, volume, MAX_PATH), "Resolve Windows volume GUID");
        std::wstring device(volume);
        device.pop_back();
        Handle h(
            CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        win_check(h.value != INVALID_HANDLE_VALUE, "Open Windows volume");
        std::array<unsigned char, 4096> bytes{};
        DWORD                           count = 0;
        win_check(DeviceIoControl(h.value, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, bytes.data(),
                                  static_cast<DWORD>(bytes.size()), &count, nullptr),
                  "Map Windows volume disks");
        const auto *extents = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(bytes.data());
        require(count >= offsetof(VOLUME_DISK_EXTENTS, Extents) && extents->NumberOfDiskExtents > 0 &&
                    extents->NumberOfDiskExtents <= 32 &&
                    count >=
                        offsetof(VOLUME_DISK_EXTENTS, Extents) + extents->NumberOfDiskExtents * sizeof(DISK_EXTENT),
                "Ambiguous Windows disk extents");
        for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i)
            running_disks.insert(extents->Extents[i].DiskNumber);
    } catch (const Error &) {
        return result;
    }
    bool boot_seen = false, system_seen = false, complete = true;
    for (const auto &object : wmi_.query(L"SELECT * FROM MSFT_Disk")) {
        Disk disk;
        try {
            disk            = describe(object.Get());
            const bool boot = boolean(object.Get(), L"IsBoot"), system = boolean(object.Get(), L"IsSystem");
            boot_seen |= boot;
            system_seen |= system;
            if (running_disks.contains(disk.number))
                disk.protected_disk = disk.boot_chain = true;
        } catch (const Error &e) {
            complete            = false;
            disk.number         = static_cast<unsigned>(number(object.Get(), L"Number"));
            disk.identity.model = text_property(object.Get(), L"FriendlyName");
            disk.identity.size  = number(object.Get(), L"Size");
            disk.refusal        = e.what();
            disk.protected_disk = true;
        }
        result.disks.push_back(disk);
    }
    for (auto number : running_disks)
        if (std::ranges::none_of(result.disks, [number](const auto &disk) { return disk.number == number; }))
            complete = false;
    result.protection_complete = complete && boot_seen && system_seen && !result.disks.empty();
    if (resize_bounds && result.protection_complete) {
        for (auto &disk : result.disks) {
            if (disk.protected_disk)
                continue;
            for (auto &p : disk.partitions)
                if (p.filesystem == "NTFS" && p.healthy && p.writable) {
                    try {
                        Wmi  encryption(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
                        auto encrypted = encryption.query(L"SELECT * FROM Win32_EncryptableVolume");
                        bool clear     = true;
                        for (auto &volume : encrypted) {
                            if (lower(text_property(volume.Get(), L"DeviceID")) == lower(p.volume_path)) {
                                const auto state = encryption.call(volume.Get(), L"GetConversionStatus");
                                clear            = number(state.Get(), L"ConversionStatus") == 0;
                            }
                        }
                        p.encryption_clear = clear;
                        auto matches       = wmi_.query(L"SELECT * FROM MSFT_Partition WHERE DiskNumber=" +
                                                        std::to_wstring(disk.number));
                        for (auto &obj : matches)
                            if (normalized_guid(text_property(obj.Get(), L"Guid")) == p.guid) {
                                auto sizes     = wmi_.call(obj.Get(), L"GetSupportedSize");
                                p.minimum      = number(sizes.Get(), L"SizeMin");
                                p.maximum      = number(sizes.Get(), L"SizeMax");
                                p.resize_known = true;
                            }
                    } catch (const Error &) {
                        p.resize_known     = false;
                        p.encryption_clear = false;
                    }
                }
        }
    }
    return result;
}
static ComPtr<IWbemClassObject> find_disk(Wmi &wmi, const Disk &disk) {
    auto matches = wmi.query(L"SELECT * FROM MSFT_Disk WHERE Number=" + std::to_wstring(disk.number));
    require(matches.size() == 1 && normalized_guid(text_property(matches[0].Get(), L"Guid")) == disk.identity.guid &&
                text_property(matches[0].Get(), L"UniqueId") == disk.identity.unique_id &&
                lower(text_property(matches[0].Get(), L"Path")) == disk.identity.device_path,
            "WMI disk identity changed");
    return matches.front();
}
static ComPtr<IWbemClassObject> find_partition(Wmi &wmi, const Disk &disk, const Partition &p) {
    auto                     objects = wmi.query(assoc(disk.wmi_path, L"MSFT_DiskToPartition"));
    ComPtr<IWbemClassObject> match;
    for (auto &object : objects)
        if (normalized_guid(text_property(object.Get(), L"Guid")) == p.guid) {
            require(!match && number(object.Get(), L"Offset") == p.range.offset &&
                        number(object.Get(), L"Size") == p.range.size,
                    "Partition identity or range changed");
            match = object;
        }
    require(match, "Partition disappeared");
    return match;
}
void Storage::resize(const Disk &disk, const Operation &op) {
    auto p = std::ranges::find(disk.partitions, op.partition_guid, &Partition::guid);
    require(p != disk.partitions.end() && p->range.size == op.old_size && op.range.size < op.old_size,
            "Invalid resize source");
    auto object = find_partition(wmi_, disk, *p);
    auto sizes  = wmi_.call(object.Get(), L"GetSupportedSize");
    require(op.range.size >= number(sizes.Get(), L"SizeMin") && op.range.size <= number(sizes.Get(), L"SizeMax"),
            "Shrink limit changed");
    wmi_.call(object.Get(), L"Resize", {{"Size", op.range.size}});
}
void Storage::create(const Disk &disk, const Operation &op) {
    auto object = find_disk(wmi_, disk);
    wmi_.call(object.Get(), L"CreatePartition",
              {{"Size", op.range.size},
               {"Offset", op.range.offset},
               {"Alignment", MiB},
               {"GptType", "{" + op.type + "}"},
               {"AssignDriveLetter", false},
               {"IsHidden", false}});
}
void Storage::format(const Disk &disk, const Partition &partition, const Operation &op) {
    auto object  = find_partition(wmi_, disk, partition);
    auto volumes = wmi_.query(assoc(text_property(object.Get(), L"__PATH"), L"MSFT_PartitionToVolume"));
    require(volumes.size() == 1 && partition.range == op.range && partition.type == op.type &&
                (partition.filesystem.empty() || partition.filesystem == "RAW"),
            "Refusing to format an existing filesystem");
    wmi_.call(volumes.front().Get(), L"Format",
              {{"FileSystem", op.filesystem}, {"FileSystemLabel", op.label}, {"Full", false}, {"Force", false}});
}
void Storage::mount(const Disk &disk, const Partition &partition, const std::filesystem::path &path) {
    auto object = find_partition(wmi_, disk, partition);
    wmi_.call(object.Get(), L"AddAccessPath", {{"AccessPath", utf8(path.wstring()) + "\\"}});
}
void Storage::unmount(const Partition &partition, const std::filesystem::path &path) {
    auto objects = wmi_.query(L"SELECT * FROM MSFT_Partition");
    for (auto &object : objects)
        if (text_property(object.Get(), L"__PATH") == partition.wmi_path &&
            normalized_guid(text_property(object.Get(), L"Guid")) == partition.guid) {
            wmi_.call(object.Get(), L"RemoveAccessPath", {{"AccessPath", utf8(path.wstring()) + "\\"}});
            return;
        }
    throw Error("Mount cleanup requires manual inspection: partition identity changed");
}
std::string stable_id(const Identity &identity) {
    const auto bytes = Json(identity).dump();
    return hash_bytes({reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()});
}
}
