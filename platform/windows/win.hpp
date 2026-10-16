#pragma once
#include "core/model.hpp"
#include <windows.h>
#include <filesystem>
#include <span>
#include <utility>

namespace diskboot {
std::string  utf8(const std::wstring &text);
std::wstring wide(const std::string &text);
std::string  lower(std::string text);
void         win_check(bool result, const std::string &context);
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    Handle(const Handle &)            = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&h) noexcept : value(std::exchange(h.value, INVALID_HANDLE_VALUE)) {}
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value)
            CloseHandle(value);
    }
};
std::string           hash_bytes(std::span<const unsigned char> data);
std::string           hash_file(const std::filesystem::path &path);
std::string           hash_handle(HANDLE handle);
void                  durable_json(const std::filesystem::path &path, const Json &value, bool exclusive);
Json                  read_json(const std::filesystem::path &path);
std::filesystem::path state_directory();
struct Firmware {
    bool        uefi        = false;
    std::string secure_boot = "unknown";
    Bytes       ram         = 0;
};
Firmware firmware();
}
