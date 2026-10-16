#pragma once
#include "platform/windows/win.hpp"
#include <map>

namespace diskboot {
class IsoImage {
    struct Entry {
        Bytes offset, size;
        bool  directory;
        bool  continuation = false;
    };
    Handle                       handle_;
    Bytes                        length_ = 0;
    std::map<std::string, Entry> files_;
    std::vector<unsigned char>   read(Bytes offset, Bytes size);
    void                         scan();

public:
    Iso metadata;
    explicit IsoImage(const std::filesystem::path &path);
    void                       copy_iso(const std::filesystem::path &destination);
    void                       copy_boot_file(const std::string &name, const std::filesystem::path &destination);
    void                       extract_efi(const std::filesystem::path &destination);
    std::vector<unsigned char> file(const std::string &name, Bytes maximum);
};
std::string ubuntu_config(const std::string &marker);
void        check_candidate_environment(const Iso &iso, const Firmware &host, const std::string &guest_secure_boot,
                                        Bytes guest_ram);
}
