#include "virtual_backend.hpp"
#include <virtdisk.h>
#include <winioctl.h>
#include <array>

namespace diskboot {
VirtualBackend::VirtualBackend(const std::filesystem::path& path) {
    auto extension = lower(utf8(path.extension().wstring()));
    require(extension == ".vhd" || extension == ".vhdx", "Destructive execution requires an attached disposable VHD/VHDX");
    VIRTUAL_STORAGE_TYPE type{static_cast<ULONG>(extension == ".vhdx" ? VIRTUAL_STORAGE_TYPE_DEVICE_VHDX : VIRTUAL_STORAGE_TYPE_DEVICE_VHD),
                              VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
    OPEN_VIRTUAL_DISK_PARAMETERS parameters{}; parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    auto status = OpenVirtualDisk(&type, path.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO, OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &virtual_disk_.value);
    require(status == ERROR_SUCCESS, "Cannot open disposable virtual disk (Win32 " + std::to_string(status) + ")");
}
void VirtualBackend::authorize(const Identity& identity) {
    require(identity.bus == 14 || identity.bus == 15, "Refusing destructive test on a physical disk");
    std::array<wchar_t, 1024> path{}; ULONG size = sizeof(path);
    auto status = GetVirtualDiskPhysicalPath(virtual_disk_.value, &size, path.data());
    require(status == ERROR_SUCCESS, "Disposable VHD is not attached");
    Handle disk(CreateFileW(path.data(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    win_check(disk.value != INVALID_HANDLE_VALUE, "Resolve VHD physical device");
    STORAGE_DEVICE_NUMBER device{}; DWORD count = 0;
    win_check(DeviceIoControl(disk.value, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &device, sizeof(device), &count, nullptr), "Resolve VHD device number");
    require(count == sizeof(device) && device.DeviceType == FILE_DEVICE_DISK, "VHD mapping is not a disk");
    const auto snapshot = storage_.inventory(false);
    const auto& resolved = resolve(snapshot, identity);
    require(snapshot.protection_complete && !resolved.boot_chain && resolved.number == device.DeviceNumber,
            "Target is not the explicitly supplied VHD or is in the Windows boot chain");
}
}
