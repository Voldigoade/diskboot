#include "iso.hpp"
#include <array>
#include <deque>
#include <set>

namespace diskboot {
static std::uint32_t both32(const unsigned char* p) {
    auto le = std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
    auto be = std::uint32_t(p[7]) | std::uint32_t(p[6]) << 8 | std::uint32_t(p[5]) << 16 | std::uint32_t(p[4]) << 24;
    require(le == be, "Conflicting ISO byte orders"); return le;
}
std::vector<unsigned char> IsoImage::read(Bytes offset, Bytes size) {
    require(size <= 32 * MiB && add(offset, size) <= length_, "ISO extent outside file or metadata limit");
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    win_check(SetFilePointerEx(handle_.value, position, nullptr, FILE_BEGIN), "Seek ISO");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    DWORD count = 0;
    win_check(ReadFile(handle_.value, bytes.data(), static_cast<DWORD>(size), &count, nullptr) && count == size, "Read ISO");
    return bytes;
}
IsoImage::IsoImage(const std::filesystem::path& path)
    : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
    win_check(handle_.value != INVALID_HANDLE_VALUE, "Open ISO read-only");
    BY_HANDLE_FILE_INFORMATION info{};
    win_check(GetFileInformationByHandle(handle_.value, &info), "ISO file identity");
    require(!(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "ISO must be a regular file");
    length_ = Bytes(info.nFileSizeHigh) << 32 | info.nFileSizeLow;
    require(length_ >= 19 * 2048 && length_ <= 128 * GiB, "ISO size outside supported limits");
    metadata.path = utf8(std::filesystem::absolute(path).wstring()); metadata.size = length_;
    metadata.sha256 = hash_handle(handle_.value);
    scan();
    metadata.distribution = "Unknown"; metadata.profile = "unsupported";
    metadata.limitation = "No validated boot profile for this ISO";
    if (files_.contains(".disk/info")) {
        auto bytes = file(".disk/info", 4096);
        std::string label(bytes.begin(), bytes.end());
        require(label.find_first_of("\r\n") != 0 && label.find('\0') == std::string::npos, "Malformed ISO identity");
        metadata.version = label.substr(0, label.find_first_of("\r\n"));
        const auto name = lower(label);
        if (name.starts_with("kubuntu ")) metadata.distribution = "Kubuntu";
        else if (name.starts_with("ubuntu ")) metadata.distribution = "Ubuntu";
        if ((metadata.distribution == "Ubuntu" || metadata.distribution == "Kubuntu") && name.find("amd64") != std::string::npos &&
            files_.contains("casper/vmlinuz") && files_.contains("casper/initrd") &&
            files_.contains("efi/boot/bootx64.efi") && files_.contains("efi/boot/grubx64.efi")) {
            metadata.profile = "ubuntu-casper-loopback"; metadata.candidate = true;
            metadata.ram_required = add(add(length_, length_ / 2), 4 * GiB);
            metadata.limitation = "Candidate only: shim/GRUB relocation, toram source release, same-disk installation, and Secure Boot have not been validated in a UEFI VM";
        }
    }
    if (files_.contains("boot/x86_64/loader/linux") && files_.contains("boot/x86_64/loader/initrd")) {
        metadata.distribution = "openSUSE-compatible layout"; metadata.profile = "opensuse-unsupported";
        metadata.limitation = "openSUSE Leap/Tumbleweed source discovery and same-disk installer behavior are not validated; Ubuntu parameters are not applicable";
    }
}
void IsoImage::scan() {
    std::vector<unsigned char> primary;
    bool terminated = false;
    for (unsigned i = 16; i < 80; ++i) {
        auto descriptor = read(Bytes(i) * 2048, 2048);
        require(std::string(descriptor.begin() + 1, descriptor.begin() + 6) == "CD001" && descriptor[6] == 1, "Invalid ISO9660 descriptor");
        if (descriptor[0] == 1) { require(primary.empty(), "Duplicate ISO primary descriptor"); primary = descriptor; }
        if (descriptor[0] == 255) { terminated = true; break; }
    }
    require(terminated && !primary.empty(), "Missing ISO descriptor terminator or primary descriptor");
    require(primary[128] == 0 && primary[129] == 8 && primary[130] == 8 && primary[131] == 0, "Unsupported ISO block size");
    const Bytes declared = Bytes(both32(primary.data() + 80)) * 2048;
    require(declared <= length_ && declared >= 19 * 2048, "Invalid ISO volume length");
    auto entry = [&](const unsigned char* record) {
        require(record[0] >= 34 && record[1] == 0 && record[26] == 0 && record[27] == 0, "Unsupported ISO record");
        Entry e{Bytes(both32(record + 2)) * 2048, both32(record + 10), (record[25] & 2) != 0};
        require(add(e.offset, e.size) <= declared, "ISO record outside volume"); return e;
    };
    std::deque<std::pair<std::string, Entry>> pending{{"", entry(primary.data() + 156)}};
    std::set<Bytes> directories;
    Bytes metadata_bytes = 0;
    while (!pending.empty()) {
        auto [prefix, directory] = pending.front(); pending.pop_front();
        require(directory.directory && directories.insert(directory.offset).second, "Cyclic or aliased ISO directory");
        metadata_bytes = add(metadata_bytes, directory.size);
        require(metadata_bytes <= 32 * MiB, "ISO metadata limit exceeded");
        auto data = read(directory.offset, directory.size);
        for (std::size_t offset = 0; offset < data.size();) {
            const unsigned length = data[offset];
            if (!length) { offset = static_cast<std::size_t>(align_up(offset + 1, 2048)); continue; }
            require(length >= 34 && offset + length <= data.size() && offset % 2048 + length <= 2048, "Truncated ISO directory entry");
            const auto* r = data.data() + offset;
            require(r[32] && 33u + r[32] <= length, "Invalid ISO filename length");
            if (!(r[32] == 1 && r[33] <= 1)) {
                std::string name(reinterpret_cast<const char*>(r + 33), r[32]);
                if (name.ends_with(";1")) name.resize(name.size() - 2);
                name = lower(name);
                require(name != "." && name != ".." && name.find_first_of("/\\:\0", 0, 4) == std::string::npos && name.find('\0') == std::string::npos, "Unsafe ISO path");
                require(std::ranges::all_of(name, [](unsigned char c) { return c >= 32 && c < 127; }), "Unsupported ISO filename encoding");
                const auto full = prefix + name;
                auto e = entry(r);
                require(!(r[25] & 0x80), "Multi-extent ISO files are not supported");
                require(files_.size() < 100000 && full.size() <= 512 && std::ranges::count(full, '/') < 16, "ISO entry limits exceeded");
                require(files_.emplace(full, e).second, "Duplicate ISO path");
                if (e.directory) pending.emplace_back(full + "/", e);
            }
            offset += length;
        }
    }
}
std::vector<unsigned char> IsoImage::file(const std::string& name, Bytes maximum) {
    auto it = files_.find(name);
    require(it != files_.end() && !it->second.directory && it->second.size <= maximum, "Missing or oversized ISO boot component");
    return read(it->second.offset, it->second.size);
}
void IsoImage::copy_iso(const std::filesystem::path& destination) {
    Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr));
    win_check(output.value != INVALID_HANDLE_VALUE, "Create staging ISO");
    win_check(SetFilePointerEx(handle_.value, {}, nullptr, FILE_BEGIN), "Seek ISO source");
    std::vector<unsigned char> bytes(MiB);
    Bytes copied = 0;
    while (copied < length_) {
        DWORD count = 0, written = 0;
        win_check(ReadFile(handle_.value, bytes.data(), static_cast<DWORD>(std::min<Bytes>(MiB, length_ - copied)), &count, nullptr) && count, "Read ISO source");
        win_check(WriteFile(output.value, bytes.data(), count, &written, nullptr) && written == count, "Copy staging ISO");
        copied += count;
    }
    win_check(FlushFileBuffers(output.value), "Flush staging ISO");
}
void IsoImage::extract_efi(const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination);
    for (const auto& name : {"bootx64.efi", "grubx64.efi", "mmx64.efi"}) {
        const auto source = std::string("efi/boot/") + name;
        if (!files_.contains(source) && std::string(name) == "mmx64.efi") continue;
        auto bytes = file(source, 32 * MiB);
        require(bytes.size() > 64 && bytes[0] == 'M' && bytes[1] == 'Z', "Invalid EFI image header");
        auto path = destination / name;
        Handle output(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr));
        win_check(output.value != INVALID_HANDLE_VALUE, "Create EFI file");
        DWORD written = 0;
        win_check(WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), "Write EFI file");
        win_check(FlushFileBuffers(output.value), "Flush EFI file");
    }
}
std::string ubuntu_config(const std::string& marker) {
    require(marker.size() == 64 && std::ranges::all_of(marker, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "Invalid recovery marker");
    return "set timeout=10\nsearch --no-floppy --file --set=root /diskboot/" + marker +
        ".marker\nset isofile=/diskboot/installer.iso\nmenuentry 'DiskBoot Ubuntu installer (unverified laboratory profile)' {\n"
        "  loopback loop $isofile\n  linux (loop)/casper/vmlinuz boot=casper iso-scan/filename=$isofile toram noprompt noeject ---\n"
        "  initrd (loop)/casper/initrd\n}\n";
}
}
