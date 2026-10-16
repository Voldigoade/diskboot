#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace diskboot {
using Json                            = nlohmann::json;
using Bytes                           = std::uint64_t;
inline constexpr Bytes       MiB      = 1024 * 1024;
inline constexpr Bytes       GiB      = 1024 * MiB;
inline constexpr const char *EspType  = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";
inline constexpr const char *DataType = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7";

struct Error : std::runtime_error {
    using runtime_error::runtime_error;
};
void  require(bool condition, const std::string &message);
Bytes add(Bytes a, Bytes b);
Bytes align_up(Bytes value, Bytes alignment = MiB);
Bytes parse_size(const std::string &value);
struct Range {
    Bytes offset = 0, size = 0;
    Bytes end() const {
        return add(offset, size);
    }
    bool contains(const Range &other) const;
    bool overlaps(const Range &other) const;
    bool operator==(const Range &) const = default;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Range, offset, size)
struct Identity {
    std::string guid, unique_id, serial, device_path, model;
    Bytes       size           = 0;
    unsigned    logical_sector = 0, physical_sector = 0, bus = 0;
    bool        operator==(const Identity &) const = default;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Identity, guid, unique_id, serial, device_path, model, size, logical_sector,
                                   physical_sector, bus)
struct Partition {
    std::string guid, type, filesystem, label, volume_path, wmi_path;
    Range       range;
    Bytes       attributes = 0, minimum = 0, maximum = 0;
    unsigned    number = 0;
    bool        boot = false, system = false, healthy = false, writable = false;
    bool        resize_known = false, encryption_clear = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Partition, guid, type, filesystem, label, volume_path, wmi_path, range, attributes,
                                   minimum, maximum, number, boot, system, healthy, writable, resize_known,
                                   encryption_clear)
struct Disk {
    Identity    identity;
    Range       usable;
    unsigned    number = 0;
    std::string wmi_path, refusal;
    bool gpt = false, online = false, writable = false, healthy = false, protected_disk = true, boot_chain = true;
    std::vector<Partition> partitions;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Disk, identity, usable, number, wmi_path, refusal, gpt, online, writable, healthy,
                                   protected_disk, boot_chain, partitions)
struct Inventory {
    bool              protection_complete = false;
    std::vector<Disk> disks;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Inventory, protection_complete, disks)
struct Iso {
    std::string path, sha256, distribution, version, profile, limitation;
    Bytes       size = 0, ram_required = 0;
    bool        candidate = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Iso, path, sha256, distribution, version, profile, limitation, size, ram_required,
                                   candidate)
struct Operation {
    std::string kind, partition_guid, filesystem, label, type;
    Range       range;
    Bytes       old_size = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Operation, kind, partition_guid, filesystem, label, type, range, old_size)
struct Plan {
    unsigned               schema = 1;
    Identity               target;
    Disk                   original;
    Iso                    iso;
    Range                  allocation, linux_space;
    Bytes                  padding = 0;
    std::vector<Operation> operations;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Plan, schema, target, original, iso, allocation, linux_space, padding, operations)
const Disk &resolve(const Inventory &inventory, const Identity &identity);
void        check_target(const Inventory &inventory, const Disk &disk, bool allow_virtual);
Plan        make_plan(const Inventory &inventory, const Identity &target, const Iso &iso, Bytes total,
                      const std::string &source_guid, bool allow_virtual = false);
void        validate_plan(const Plan &plan);
std::string summarize(const Plan &plan);
}
