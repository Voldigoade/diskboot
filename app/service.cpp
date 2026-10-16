#include "service.hpp"
#include <winioctl.h>
#include <algorithm>

namespace diskboot {
static Disk selected(const Inventory &inventory, const std::string &id) {
    require(id.size() == 64, "Select a complete stable disk ID from the disks command");
    const Disk *result = nullptr;
    for (const auto &disk : inventory.disks)
        if (stable_id(disk.identity) == id) {
            require(!result, "Ambiguous disk ID");
            result = &disk;
        }
    require(result, "Selected disk is unavailable");
    return *result;
}
Json disk_inventory() {
    Storage    storage;
    const auto inventory = storage.inventory(true);
    auto       host      = firmware();
    Json       disks     = Json::array();
    for (const auto &disk : inventory.disks) {
        bool        selectable = false;
        std::string refusal;
        try {
            check_target(inventory, disk, false);
            selectable = true;
        } catch (const Error &e) {
            refusal = e.what();
        }
        disks.push_back(
            {{"id", stable_id(disk.identity)}, {"disk", disk}, {"selectable", selectable}, {"reason", refusal}});
    }
    return {{"protection_complete", inventory.protection_complete},
            {"uefi", host.uefi},
            {"secure_boot", host.secure_boot},
            {"ram_bytes", host.ram},
            {"physical_preparation_enabled", false},
            {"disks", disks}};
}
Plan plan_request(const Request &request) {
    IsoImage   image(request.iso);
    Storage    storage;
    const auto inventory = storage.inventory(true);
    const auto disk      = selected(inventory, request.disk_id);
    if (!request.lab_vhd.empty()) {
        VirtualBackend backend(request.lab_vhd);
        backend.authorize(disk.identity);
    }
    return make_plan(inventory, disk.identity, image.metadata, request.size, request.partition_guid,
                     !request.lab_vhd.empty());
}
static void write_file(const std::filesystem::path &path, const std::string &data) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr));
    win_check(file.value != INVALID_HANDLE_VALUE, "Create boot configuration");
    DWORD written = 0;
    win_check(data.size() < MAXDWORD &&
                  WriteFile(file.value, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                  written == data.size(),
              "Write boot configuration");
    win_check(FlushFileBuffers(file.value), "Flush boot configuration");
}
static void verify_volume(const Partition &partition, const Disk &disk) {
    auto device = wide(partition.volume_path);
    require(!device.empty() && device.ends_with(L'\\'), "Missing volume GUID root");
    device.pop_back();
    Handle handle(
        CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    win_check(handle.value != INVALID_HANDLE_VALUE, "Open prepared volume for extent verification");
    VOLUME_DISK_EXTENTS extents{};
    DWORD               count = 0;
    win_check(DeviceIoControl(handle.value, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &extents, sizeof(extents),
                              &count, nullptr),
              "Verify prepared volume extent");
    require(count >= sizeof(extents) && extents.NumberOfDiskExtents == 1 &&
                extents.Extents[0].DiskNumber == disk.number && extents.Extents[0].StartingOffset.QuadPart >= 0 &&
                extents.Extents[0].ExtentLength.QuadPart > 0 &&
                partition.range == Range{static_cast<Bytes>(extents.Extents[0].StartingOffset.QuadPart),
                                         static_cast<Bytes>(extents.Extents[0].ExtentLength.QuadPart)},
            "Prepared volume maps outside its approved partition");
}
std::filesystem::path prepare_virtual(const Request &request, const Plan &approved,
                                      const std::function<void(const std::string &)> &progress) {
    require(!request.lab_vhd.empty(),
            "Physical preparation is disabled: no installer profile has passed UEFI VM validation");
    IsoImage image(request.iso);
    require(image.metadata.sha256 == approved.iso.sha256 && image.metadata.size == approved.iso.size,
            "ISO changed after review");
    const auto host = firmware();
    check_candidate_environment(image.metadata, host, request.guest_secure_boot, request.guest_ram);
    VirtualBackend backend(request.lab_vhd);
    backend.authorize(approved.target);
    const auto media_id     = stable_id(approved.target);
    const auto directory    = state_directory();
    const auto journal_path = directory / L"transactions" / (stable_id(approved.target) + ".json");
    require(!std::filesystem::exists(journal_path),
            "A transaction already exists for this disk; inspect it before any new operation");
    Handle lock(CreateFileW((directory / L"execution.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    win_check(lock.value != INVALID_HANDLE_VALUE, "Another DiskBoot operation is active");
    bool first   = true;
    auto persist = [&](const Json &journal) {
        durable_json(journal_path, journal, first);
        first = false;
        progress(journal.at("phase").get<std::string>());
    };
    auto state                = execute_partitions(approved, backend, persist);
    state.journal["firmware"] = {{"uefi", host.uefi}, {"secure_boot", host.secure_boot}, {"ram_bytes", host.ram}};
    state.journal["requested_guest"] = {
        {"secure_boot", request.guest_secure_boot}, {"ram_bytes", request.guest_ram}, {"verified", false}};
    state.journal["files"] = Json::array();
    auto checkpoint        = [&](const std::string &phase) {
        state.journal["phase"]    = phase;
        state.journal["expected"] = state.expected;
        persist(state.journal);
    };
    try {
        for (const auto &op : approved.operations) {
            if (op.kind != "create")
                continue;
            auto current = revalidate(backend, approved.target, state.expected);
            auto it      = std::ranges::find(current.partitions, op.range, &Partition::range);
            require(it != current.partitions.end(), "Prepared partition missing");
            const auto partition = *it;
            const auto mount     = std::filesystem::path(wide(partition.volume_path));
            verify_volume(partition, current);
            auto before_write = [&] {
                const auto verified = revalidate(backend, approved.target, state.expected);
                require(approved.allocation.contains(partition.range), "Boot files outside approved allocation");
                verify_volume(partition, verified);
            };
            before_write();
            checkpoint("boot-files-intent");
            std::vector<std::filesystem::path> files;
            if (op.type == EspType) {
                image.extract_efi(mount / L"EFI" / L"BOOT");
                for (const auto &file : std::filesystem::directory_iterator(mount / L"EFI" / L"BOOT"))
                    files.push_back(std::filesystem::relative(file.path(), mount));
                const auto boot_folder = std::filesystem::path(L"diskboot") / media_id;
                before_write();
                std::filesystem::create_directories(mount / boot_folder);
                image.copy_boot_file("casper/vmlinuz", mount / boot_folder / L"vmlinuz");
                before_write();
                image.copy_boot_file("casper/initrd", mount / boot_folder / L"initrd");
                files.push_back(boot_folder / L"vmlinuz");
                files.push_back(boot_folder / L"initrd");
                const auto config = ubuntu_config(media_id);
                for (const auto &folder : {std::filesystem::path(L"EFI\\BOOT"), std::filesystem::path(L"EFI\\ubuntu"),
                                           std::filesystem::path(L"boot\\grub")}) {
                    before_write();
                    std::filesystem::create_directories(mount / folder);
                    write_file(mount / folder / L"grub.cfg", config);
                    files.push_back(folder / L"grub.cfg");
                }
            } else {
                const auto recovery_folder = std::filesystem::path(L"diskboot") / media_id;
                std::filesystem::create_directories(mount / recovery_folder);
                image.copy_iso(mount / recovery_folder / L"installer.iso");
                require(hash_file(mount / recovery_folder / L"installer.iso") == image.metadata.sha256,
                        "Staged ISO hash mismatch");
                before_write();
                files.push_back(recovery_folder / L"installer.iso");
                durable_json(mount / recovery_folder / L"plan.json", Json(approved), true);
                files.push_back(recovery_folder / L"plan.json");
            }
            for (const auto &file : files) {
                before_write();
                state.journal["files"].push_back({{"partition_guid", partition.guid},
                                                  {"volume", partition.volume_path},
                                                  {"relative", utf8(file.wstring())},
                                                  {"sha256", hash_file(mount / file)}});
            }
            checkpoint("boot-files-verified");
        }
        revalidate(backend, approved.target, state.expected);
        state.journal["boot_tested"] = false;
        checkpoint("laboratory-media-verified");
    } catch (const std::exception &e) {
        state.journal["failure"] = e.what();
        checkpoint("needs-inspection");
        throw;
    }
    return journal_path;
}
Json transaction_status() {
    Json result = Json::array();
    for (const auto &entry : std::filesystem::directory_iterator(state_directory() / L"transactions")) {
        if (entry.path().extension() != ".json")
            continue;
        try {
            const auto journal = read_json(entry.path());
            result.push_back(
                {{"journal", utf8(entry.path().wstring())},
                 {"phase", journal.at("phase")},
                 {"recovery", "Inspect recorded and actual state. Automatic resume and rollback are disabled."}});
        } catch (const std::exception &e) {
            result.push_back({{"journal", utf8(entry.path().wstring())}, {"error", e.what()}});
        }
    }
    return result;
}
Json verify_transaction(const std::filesystem::path &path) {
    auto journal = read_json(path);
    auto plan    = journal.at("plan").get<Plan>();
    validate_plan(plan);
    auto    expected = journal.at("expected").get<Disk>();
    Storage storage;
    auto    inventory = storage.inventory(false);
    require(inventory.protection_complete, "Protection evidence unavailable");
    verify_layout(resolve(inventory, plan.target), expected);
    require(journal.at("phase") == "laboratory-media-verified",
            "Transaction is unfinished; inspect intent before continuing");
    for (const auto &file : journal.at("files")) {
        const auto guid = file.at("partition_guid").get<std::string>();
        const auto p    = std::ranges::find(expected.partitions, guid, &Partition::guid);
        require(p != expected.partitions.end() && plan.allocation.contains(p->range), "Unowned journal file");
        const auto relative = std::filesystem::path(wide(file.at("relative").get<std::string>()));
        require(!relative.is_absolute() && !relative.has_root_path() &&
                    std::ranges::none_of(relative, [](const auto &part) { return part == L".."; }),
                "Invalid journal file path");
        require(file.at("volume").get<std::string>() == p->volume_path && !p->volume_path.empty(),
                "Journal volume identity mismatch");
        require(hash_file(std::filesystem::path(wide(p->volume_path)) / relative) ==
                    file.at("sha256").get<std::string>(),
                "Prepared file hash mismatch");
    }
    return {{"layout_verified", true}, {"file_hashes_verified", true}, {"boot_verified", false}};
}
}
