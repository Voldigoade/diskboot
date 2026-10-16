#include "model.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <sstream>

namespace diskboot {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw Error(message);
}
Bytes add(Bytes a, Bytes b) {
    require(b <= std::numeric_limits<Bytes>::max() - a, "Byte range overflow");
    return a + b;
}
Bytes align_up(Bytes value, Bytes alignment) {
    require(alignment != 0, "Invalid alignment");
    return add(value, (alignment - value % alignment) % alignment);
}
Bytes parse_size(const std::string &text) {
    Bytes multiplier = 1;
    auto  digits     = text;
    for (auto [suffix, scale] : {std::pair{"GiB", GiB}, {"GB", Bytes{1000000000}}, {"MiB", MiB}}) {
        if (text.ends_with(suffix)) {
            digits.resize(text.size() - std::string(suffix).size());
            multiplier = scale;
            break;
        }
    }
    Bytes value       = 0;
    auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    require(error == std::errc{} && end == digits.data() + digits.size() && value > 0,
            "Size must be a positive integer followed by GB, GiB, MiB, or bytes");
    require(value <= std::numeric_limits<Bytes>::max() / multiplier, "Size overflow");
    return value * multiplier;
}
bool Range::contains(const Range &other) const {
    return other.size && offset <= other.offset && other.end() <= end();
}
bool Range::overlaps(const Range &other) const {
    return size && other.size && offset < other.end() && other.offset < end();
}

const Disk &resolve(const Inventory &inventory, const Identity &id) {
    const Disk *match = nullptr;
    for (const auto &disk : inventory.disks) {
        if (disk.identity.guid == id.guid || disk.identity.unique_id == id.unique_id) {
            require(disk.identity == id && !match, "Disk identity changed or is duplicated");
            match = &disk;
        }
    }
    require(match, "Target disk disappeared");
    return *match;
}
void check_target(const Inventory &inventory, const Disk &disk, bool allow_virtual) {
    require(inventory.protection_complete, "Windows boot-chain protection could not be established");
    require(!disk.protected_disk && !disk.boot_chain, "Protected Windows disk or existing EFI partition");
    require(disk.refusal.empty(), disk.refusal);
    require(disk.gpt && disk.online && disk.writable && disk.healthy,
            "Target must be a healthy, online, writable GPT disk");
    const auto &id = disk.identity;
    require(!id.guid.empty() && !id.unique_id.empty() && !id.device_path.empty() && !id.model.empty(),
            "Incomplete disk identity");
    require(id.logical_sector == 512 && (id.physical_sector == 512 || id.physical_sector == 4096),
            "Untested sector geometry");
    require(id.bus == 11 || id.bus == 17 || (allow_virtual && (id.bus == 14 || id.bus == 15)),
            "Unsupported bus; SATA/NVMe required");
    require(disk.usable.size && disk.usable.end() <= id.size && disk.usable.offset > 0, "Invalid GPT usable bounds");
    std::set<std::string> guids;
    Bytes                 end        = disk.usable.offset;
    auto                  partitions = disk.partitions;
    std::ranges::sort(partitions, {}, [](const auto &p) { return p.range.offset; });
    for (const auto &p : partitions) {
        require(!p.guid.empty() && guids.insert(p.guid).second && disk.usable.contains(p.range) &&
                    p.range.offset >= end,
                "Ambiguous partition layout");
        require(!p.boot && !p.system && p.type != EspType, "Existing boot partition is protected");
        require(p.type == DataType || p.type == "e3c9e316-0b5c-4db8-817d-f92df00215ae",
                "Unsupported target partition type");
        end = p.range.end();
    }
    resolve(inventory, id);
}
Plan make_plan(const Inventory &inventory, const Identity &target, const Iso &iso, Bytes total,
               const std::string &source_guid, bool allow_virtual) {
    const auto &disk = resolve(inventory, target);
    check_target(inventory, disk, allow_virtual);
    require(iso.candidate && iso.profile == "ubuntu-casper-loopback" && iso.sha256.size() == 64,
            iso.limitation.empty() ? "Unsupported ISO" : iso.limitation);
    require(total && total % target.logical_sector == 0, "Allocation must be a whole number of sectors");
    Plan plan;
    plan.target     = target;
    plan.original   = disk;
    plan.iso        = iso;
    auto partitions = disk.partitions;
    std::ranges::sort(partitions, {}, [](const auto &p) { return p.range.offset; });
    Bytes cursor = disk.usable.offset;
    for (std::size_t i = 0; i <= partitions.size(); ++i) {
        auto       boundary = i == partitions.size() ? disk.usable.end() : partitions[i].range.offset;
        const auto aligned  = align_up(cursor);
        if (aligned <= boundary && total <= boundary - aligned) {
            plan.allocation = {aligned, total};
            break;
        }
        if (i < partitions.size())
            cursor = partitions[i].range.end();
    }
    if (!plan.allocation.size) {
        require(!source_guid.empty(), "No sufficient unallocated extent; select an NTFS partition to shrink");
        const Partition *source = nullptr;
        for (const auto &p : partitions)
            if (p.guid == source_guid)
                source = &p;
        require(source, "Selected partition does not exist");
        require(source->filesystem == "NTFS" && source->healthy && source->writable && source->encryption_clear &&
                    source->attributes == 0,
                "Source must be healthy, writable, unencrypted NTFS without special attributes");
        require(source->resize_known && source->minimum > 0 && source->minimum <= source->range.size &&
                    source->maximum >= source->range.size,
                "Windows supported resize bounds unavailable or inconsistent");
        require(total < source->range.size && source->range.size - total >= source->minimum,
                "Requested allocation exceeds Windows shrink limit; maximum bytes: " +
                    std::to_string(source->range.size - source->minimum));
        plan.allocation = {source->range.end() - total, total};
        plan.operations.push_back({"resize",
                                   source->guid,
                                   "NTFS",
                                   source->label,
                                   source->type,
                                   {source->range.offset, source->range.size - total},
                                   source->range.size});
    }
    const Bytes esp_start   = align_up(plan.allocation.offset);
    const Bytes esp_size    = GiB;
    const Bytes stage_size  = align_up(add(add(iso.size, iso.size / 20), 256 * MiB));
    const Bytes stage_start = add(esp_start, esp_size);
    const Bytes linux_start = add(stage_start, stage_size);
    require(linux_start < plan.allocation.end() && plan.allocation.end() - linux_start >= GiB,
            "ISO, EFI, and minimum Linux space exceed total allocation");
    plan.padding = esp_start - plan.allocation.offset;
    plan.operations.push_back({"create", "", "FAT32", "DISKBOOTEFI", EspType, {esp_start, esp_size}, 0});
    plan.operations.push_back({"create", "", "NTFS", "DiskBoot Recovery", DataType, {stage_start, stage_size}, 0});
    plan.linux_space = {linux_start, plan.allocation.end() - linux_start};
    validate_plan(plan);
    return plan;
}
void validate_plan(const Plan &p) {
    require(p.schema == 1 && p.target == p.original.identity, "Invalid plan identity or version");
    Inventory inventory{true, {p.original}};
    check_target(inventory, p.original, true);
    require(p.original.usable.contains(p.allocation) && p.allocation.contains(p.linux_space) && p.padding < MiB,
            "Plan outside GPT usable range");
    require(p.operations.size() == 2 || p.operations.size() == 3, "Unexpected operations");
    Bytes accounted = p.padding;
    Bytes cursor    = add(p.allocation.offset, p.padding);
    bool  resized   = false;
    for (std::size_t i = 0; i < p.operations.size(); ++i) {
        const auto &op = p.operations[i];
        if (op.kind == "resize") {
            require(i == 0 && !resized && p.operations.size() == 3, "Invalid resize order");
            auto it = std::ranges::find(p.original.partitions, op.partition_guid, &Partition::guid);
            require(it != p.original.partitions.end() && op.range.offset == it->range.offset &&
                        op.old_size == it->range.size && op.range.size < op.old_size &&
                        op.old_size - op.range.size == p.allocation.size && op.range.end() == p.allocation.offset &&
                        op.range.size >= it->minimum && it->resize_known && it->filesystem == "NTFS" && it->healthy &&
                        it->writable && it->encryption_clear && !it->attributes,
                    "Resize is outside approved source tail");
            resized = true;
        } else {
            require(op.kind == "create" && p.allocation.contains(op.range) && op.range.offset == cursor &&
                        op.range.offset % MiB == 0 && op.range.size % MiB == 0,
                    "Invalid creation range");
            bool esp = i == (resized ? 1u : 0u);
            require(op.type == (esp ? EspType : DataType) && op.filesystem == (esp ? "FAT32" : "NTFS") &&
                        op.label == (esp ? "DISKBOOTEFI" : "DiskBoot Recovery") && (esp ? op.range.size == GiB : true),
                    "Invalid partition role");
            cursor    = op.range.end();
            accounted = add(accounted, op.range.size);
        }
    }
    require(cursor == p.linux_space.offset && add(accounted, p.linux_space.size) == p.allocation.size,
            "Allocation accounting mismatch");
    for (const auto &existing : p.original.partitions) {
        auto effective = existing.range;
        if (resized && existing.guid == p.operations.front().partition_guid)
            effective.size = p.operations.front().range.size;
        require(!effective.overlaps(p.allocation), "Plan overlaps existing data");
    }
}
std::string summarize(const Plan &p) {
    std::ostringstream out;
    out << "Windows disk: protected. No partition or boot changes.\nTarget: " << p.target.model
        << "\nTotal allocation: " << p.allocation.size << " bytes\nLinux space: " << p.linux_space.size << " bytes\n";
    for (const auto &op : p.operations)
        out << op.kind << " " << op.label << " offset=" << op.range.offset << " size=" << op.range.size << " "
            << op.filesystem << "\n";
    out << "Boot profile: " << p.iso.profile << " (unverified; physical-disk preparation disabled)\n";
    return out.str();
}
}
