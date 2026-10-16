#include "win.hpp"
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace diskboot {
void win_check(bool result, const std::string &context) {
    if (!result)
        throw Error(context + " (Win32 " + std::to_string(GetLastError()) + ")");
}
std::string utf8(const std::wstring &text) {
    if (text.empty())
        return {};
    auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr,
                                    0, nullptr, nullptr);
    win_check(size > 0, "Invalid Unicode");
    std::string result(size, 0);
    win_check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                  result.data(), size, nullptr, nullptr) != 0,
              "Unicode conversion");
    return result;
}
std::wstring wide(const std::string &text) {
    if (text.empty())
        return {};
    auto size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    win_check(size > 0, "Invalid UTF-8");
    std::wstring result(size, 0);
    win_check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                  result.data(), size) != 0,
              "UTF-8 conversion");
    return result;
}
std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
class Sha256 {
    BCRYPT_ALG_HANDLE  algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_      = nullptr;

public:
    Sha256() {
        require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
                "SHA-256 provider unavailable");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            throw Error("SHA-256 initialization failed");
        }
    }
    ~Sha256() {
        BCryptDestroyHash(hash_);
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void update(std::span<const unsigned char> bytes) {
        require(bytes.size() <= MAXDWORD &&
                    BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0,
                "SHA-256 update failed");
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        require(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "SHA-256 failed");
        std::ostringstream out;
        for (auto c : digest)
            out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        return out.str();
    }
};
std::string hash_bytes(std::span<const unsigned char> data) {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}
std::string hash_handle(HANDLE handle) {
    win_check(SetFilePointerEx(handle, {}, nullptr, FILE_BEGIN), "Seek hash source");
    Sha256                     hash;
    std::vector<unsigned char> buffer(MiB);
    DWORD                      count;
    do {
        win_check(ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr),
                  "Read hash source");
        hash.update({buffer.data(), count});
    } while (count);
    return hash.finish();
}
std::string hash_file(const std::filesystem::path &path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    win_check(file.value != INVALID_HANDLE_VALUE, "Open hash source");
    return hash_handle(file.value);
}
void durable_json(const std::filesystem::path &path, const Json &value, bool exclusive) {
    const auto data   = value.dump(2) + "\n";
    auto       output = path;
    if (!exclusive)
        output += L".new";
    Handle file(CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr));
    win_check(file.value != INVALID_HANDLE_VALUE, "Create journal (existing or interrupted journal must be inspected)");
    DWORD written = 0;
    win_check(data.size() < MAXDWORD &&
                  WriteFile(file.value, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                  written == data.size(),
              "Write journal");
    win_check(FlushFileBuffers(file.value), "Flush journal");
    if (!exclusive) {
        CloseHandle(file.value);
        file.value = INVALID_HANDLE_VALUE;
        win_check(MoveFileExW(output.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
                  "Commit journal");
    }
}
Json read_json(const std::filesystem::path &path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr));
    win_check(file.value != INVALID_HANDLE_VALUE, "Open journal");
    LARGE_INTEGER size{};
    win_check(GetFileSizeEx(file.value, &size), "Journal size");
    require(size.QuadPart > 0 && size.QuadPart <= 16 * MiB, "Invalid journal size");
    std::string data(static_cast<std::size_t>(size.QuadPart), 0);
    DWORD       count = 0;
    win_check(ReadFile(file.value, data.data(), static_cast<DWORD>(data.size()), &count, nullptr) &&
                  count == data.size(),
              "Read journal");
    return Json::parse(data);
}
std::filesystem::path state_directory() {
    PWSTR raw = nullptr;
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)),
            "Local application data unavailable");
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    path /= L"DiskBoot";
    std::filesystem::create_directories(path / L"transactions");
    return path;
}
Firmware firmware() {
    Firmware      result;
    FIRMWARE_TYPE type{};
    win_check(GetFirmwareType(&type), "Detect firmware");
    result.uefi = type == FirmwareTypeUefi;
    MEMORYSTATUSEX memory{sizeof(memory)};
    win_check(GlobalMemoryStatusEx(&memory), "Detect RAM");
    result.ram = memory.ullTotalPhys;
    if (!result.uefi) {
        result.secure_boot = "not-uefi";
        return result;
    }
    Handle token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token.value)) {
        TOKEN_PRIVILEGES requested{}, previous{};
        DWORD            previous_size = sizeof(previous);
        requested.PrivilegeCount       = 1;
        if (LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &requested.Privileges[0].Luid)) {
            requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            SetLastError(ERROR_SUCCESS);
            if (AdjustTokenPrivileges(token.value, FALSE, &requested, sizeof(previous), &previous, &previous_size) &&
                GetLastError() == ERROR_SUCCESS) {
                unsigned char enabled = 255, setup = 255;
                const auto   *guid         = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
                const auto    enabled_size = GetFirmwareEnvironmentVariableW(L"SecureBoot", guid, &enabled, 1);
                const auto    setup_size   = GetFirmwareEnvironmentVariableW(L"SetupMode", guid, &setup, 1);
                AdjustTokenPrivileges(token.value, FALSE, &previous, 0, nullptr, nullptr);
                if (enabled_size == 1 && setup_size == 1 && enabled <= 1 && setup <= 1 && !(enabled && setup))
                    result.secure_boot = enabled ? "enabled" : "disabled";
            }
        }
    }
    return result;
}
}
