#include "core/model.hpp"
#include "boot/iso.hpp"
#include <functional>
#include <iostream>
#include <limits>

using namespace diskboot;
static unsigned passed = 0;
static void     test(const char *name, const std::function<void()> &run) {
    try {
        run();
        ++passed;
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        std::exit(1);
    }
}
static void refuses(const std::function<void()> &run) {
    try {
        run();
    } catch (const Error &) {
        return;
    }
    throw Error("Expected refusal");
}
static Inventory fixture() {
    Disk disk;
    disk.identity = {"disk-guid", "device-id", "serial", "device-path", "Test SATA", 2048 * GiB, 512, 4096, 11};
    disk.usable   = {MiB, 2048 * GiB - 2 * MiB};
    disk.gpt = disk.online = disk.writable = disk.healthy = true;
    disk.protected_disk = disk.boot_chain = false;
    Partition p;
    p.guid       = "partition-guid";
    p.type       = DataType;
    p.filesystem = "NTFS";
    p.label      = "Games";
    p.range      = {MiB, 1500 * GiB};
    p.minimum    = 800 * GiB;
    p.maximum    = 2048 * GiB - 2 * MiB;
    p.healthy = p.writable = p.resize_known = p.encryption_clear = true;
    disk.partitions.push_back(p);
    return {true, {disk}};
}
static Iso iso() {
    Iso i;
    i.path      = "installer.iso";
    i.sha256    = std::string(64, 'a');
    i.size      = 6 * GiB;
    i.profile   = "ubuntu-casper-loopback";
    i.candidate = true;
    return i;
}
int main() {
    test("500GB total budget and existing free extent", [] {
        auto inv = fixture();
        auto p   = make_plan(inv, inv.disks[0].identity, iso(), parse_size("500GB"), "");
        require(p.allocation.size == 500000000000 && p.operations.size() == 2, "Incorrect total or unnecessary resize");
    });
    test("shrink exactly requested total", [] {
        auto  inv                  = fixture();
        auto &d                    = inv.disks[0];
        d.partitions[0].range.size = 2000 * GiB;
        auto p                     = make_plan(inv, d.identity, iso(), parse_size("500GB"), "partition-guid");
        require(p.operations.size() == 3 && p.operations[0].old_size - p.operations[0].range.size == 500000000000,
                "Incorrect shrink");
        validate_plan(p);
    });
    test("free filesystem bytes do not imply shrinkability", [] {
        auto  inv       = fixture();
        auto &d         = inv.disks[0];
        auto &part      = d.partitions[0];
        part.range.size = 2000 * GiB;
        part.minimum    = 1900 * GiB;
        refuses([&] { make_plan(inv, d.identity, iso(), 500 * GiB, part.guid); });
    });
    test("running Windows disk", [] {
        auto i                    = fixture();
        i.disks[0].protected_disk = true;
        refuses([&] { make_plan(i, i.disks[0].identity, iso(), 500 * GiB, ""); });
    });
    test("incomplete protection evidence", [] {
        auto i                = fixture();
        i.protection_complete = false;
        refuses([&] { make_plan(i, i.disks[0].identity, iso(), 500 * GiB, ""); });
    });
    test("renumbering preserves identity", [] {
        auto i            = fixture();
        auto id           = i.disks[0].identity;
        i.disks[0].number = 99;
        require(resolve(i, id).number == 99, "Disk number used as identity");
    });
    test("same GUID with different device", [] {
        auto i                     = fixture();
        auto id                    = i.disks[0].identity;
        i.disks[0].identity.serial = "replacement";
        refuses([&] { resolve(i, id); });
    });
    test("duplicate stable identity", [] {
        auto i = fixture();
        i.disks.push_back(i.disks[0]);
        refuses([&] { resolve(i, i.disks[0].identity); });
    });
    test("missing disk", [] {
        auto i  = fixture();
        auto id = i.disks[0].identity;
        i.disks.clear();
        refuses([&] { resolve(i, id); });
    });
    test("wrong partition type", [] {
        auto i                        = fixture();
        i.disks[0].partitions[0].type = "ldm";
        refuses([&] { check_target(i, i.disks[0], false); });
    });
    test("MBR disk", [] {
        auto i         = fixture();
        i.disks[0].gpt = false;
        refuses([&] { check_target(i, i.disks[0], false); });
    });
    test("existing EFI partition", [] {
        auto i                        = fixture();
        i.disks[0].partitions[0].type = EspType;
        refuses([&] { check_target(i, i.disks[0], false); });
    });
    test("unsupported sector geometry", [] {
        auto i                             = fixture();
        i.disks[0].identity.logical_sector = 4096;
        refuses([&] { check_target(i, i.disks[0], false); });
    });
    test("overlapping partitions", [] {
        auto i = fixture();
        auto p = i.disks[0].partitions[0];
        p.guid = "other";
        i.disks[0].partitions.push_back(p);
        refuses([&] { check_target(i, i.disks[0], false); });
    });
    test("encrypted source", [] {
        auto  i                          = fixture();
        auto &d                          = i.disks[0];
        d.partitions[0].range.size       = 2000 * GiB;
        d.partitions[0].encryption_clear = false;
        refuses([&] { make_plan(i, d.identity, iso(), 500 * GiB, "partition-guid"); });
    });
    test("unknown resize limits", [] {
        auto  i                      = fixture();
        auto &d                      = i.disks[0];
        d.partitions[0].range.size   = 2000 * GiB;
        d.partitions[0].resize_known = false;
        refuses([&] { make_plan(i, d.identity, iso(), 500 * GiB, "partition-guid"); });
    });
    test("oversized ISO", [] {
        auto i     = fixture();
        auto image = iso();
        image.size = 600 * GiB;
        refuses([&] { make_plan(i, i.disks[0].identity, image, 500 * GiB, ""); });
    });
    test("unknown distribution", [] {
        auto i          = fixture();
        auto image      = iso();
        image.candidate = false;
        refuses([&] { make_plan(i, i.disks[0].identity, image, 500 * GiB, ""); });
    });
    test("malicious range editing", [] {
        auto i                           = fixture();
        auto p                           = make_plan(i, i.disks[0].identity, iso(), 500 * GiB, "");
        p.operations.back().range.offset = MiB;
        refuses([&] { validate_plan(p); });
    });
    test("arithmetic overflow", [] { refuses([] { add(std::numeric_limits<Bytes>::max(), 1); }); });
    test("invalid numeric input", [] {
        for (const auto &x : {"-1GB", "0GB", "1.5GB", "500GBjunk", "18446744073709551615GB"})
            refuses([&] { parse_size(x); });
    });
    test("GRUB config rejects injection", [] {
        refuses([] { ubuntu_config("'; reboot"); });
        require(ubuntu_config(std::string(64, 'a')).find("hd0") == std::string::npos, "Fixed disk in GRUB");
    });
    test("SHA-256 known vector", [] {
        require(hash_bytes({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "Incorrect SHA-256");
    });
    std::cout << passed << " tests passed\n";
}
