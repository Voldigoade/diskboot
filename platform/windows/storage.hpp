#pragma once
#include "win.hpp"
#include <wbemidl.h>
#include <wrl/client.h>

namespace diskboot {
using Microsoft::WRL::ComPtr;
class ComRuntime {
public:
    ComRuntime();
    ~ComRuntime();
};
class Wmi {
    ComPtr<IWbemServices> services_;
public:
    explicit Wmi(const wchar_t* name_space = L"ROOT\\Microsoft\\Windows\\Storage");
    std::vector<ComPtr<IWbemClassObject>> query(const std::wstring& text);
    ComPtr<IWbemClassObject> call(IWbemClassObject* object, const wchar_t* method, const Json& args = Json::object());
};
class Storage {
    Wmi wmi_;
    Disk describe(IWbemClassObject* object);
public:
    Inventory inventory(bool resize_bounds = false);
    void resize(const Disk& disk, const Operation& op);
    void create(const Disk& disk, const Operation& op);
    void format(const Disk& disk, const Partition& partition, const Operation& op);
    void mount(const Disk& disk, const Partition& partition, const std::filesystem::path& path);
    void unmount(const Partition& partition, const std::filesystem::path& path);
};
std::string stable_id(const Identity& identity);
}
