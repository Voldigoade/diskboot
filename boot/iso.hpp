#pragma once
#include "platform/windows/win.hpp"
#include <map>

namespace diskboot {
class IsoImage {
    struct Entry { Bytes offset, size; bool directory; };
    Handle handle_;
    Bytes length_ = 0;
    std::map<std::string, Entry> files_;
    std::vector<unsigned char> read(Bytes offset, Bytes size);
    void scan();
public:
    Iso metadata;
    explicit IsoImage(const std::filesystem::path& path);
    void copy_iso(const std::filesystem::path& destination);
    void extract_efi(const std::filesystem::path& destination);
    std::vector<unsigned char> file(const std::string& name, Bytes maximum);
};
std::string ubuntu_config(const std::string& marker);
}
