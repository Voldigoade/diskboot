#include "service.hpp"
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
static void verify_mount(const std::filesystem::path &path, const Partition &partition) {
    std::wstring mount = path.wstring() + L"\\";
    wchar_t      actual[1024]{};
    win_check(GetVolumeNameForVolumeMountPointW(mount.c_str(), actual, 1024), "Verify new volume mount");
    require(!partition.volume_path.empty() && lower(utf8(actual)) == lower(partition.volume_path),
            "Mount resolves to another volume");
}
std::filesystem::path prepare_virtual(const Request &request, const Plan &approved,
                                      const std::function<void(const std::string &)> &progress) {
    require(!request.lab_vhd.empty(),
            "Physical preparation is disabled: no installer profile has passed UEFI VM validation");
    IsoImage image(request.iso);
    require(image.metadata.sha256 == approved.iso.sha256 && image.metadata.size == approved.iso.size,
            "ISO changed after review");
    const auto host = firmware();
    require(host.uefi, "UEFI host required");
    require(host.secure_boot == "disabled",
            "Laboratory profile has no verified Secure Boot chain; enabled or unknown state is refused");
    require(host.ram >= image.metadata.ram_required, "Insufficient physical RAM for candidate toram strategy");
    VirtualBackend backend(request.lab_vhd);
    backend.authorize(approved.target);
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
    state.journal["files"]    = Json::array();
    auto checkpoint           = [&](const std::string &phase) {
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
            auto       mount = directory / (stable_id(approved.target) + (op.type == EspType ? "-efi" : "-recovery"));
            require(!std::filesystem::exists(mount), "Stale mount directory requires inspection");
            std::filesystem::create_directory(mount);
            state.journal["mount"] = utf8(mount.wstring());
            checkpoint("mount-intent");
            backend.storage().mount(current, partition, mount);
            verify_mount(mount, partition);
            revalidate(backend, approved.target, state.expected);
            checkpoint("mount-verified");
            auto before_write = [&] {
                revalidate(backend, approved.target, state.expected);
                require(approved.allocation.contains(partition.range), "Boot files outside approved allocation");
                verify_mount(mount, partition);
            };
            before_write();
            checkpoint("boot-files-intent");
            std::vector<std::filesystem::path> files;
            if (op.type == EspType) {
                image.extract_efi(mount / L"EFI" / L"BOOT");
                for (const auto &file : std::filesystem::directory_iterator(mount / L"EFI" / L"BOOT"))
                    files.push_back(std::filesystem::relative(file.path(), mount));
                const auto config = ubuntu_config(image.metadata.sha256);
                for (const auto &folder : {std::filesystem::path(L"EFI/BOOT"), std::filesystem::path(L"EFI/ubuntu"),
                                           std::filesystem::path(L"boot/grub")}) {
                    before_write();
                    std::filesystem::create_directories(mount / folder);
                    write_file(mount / folder / L"grub.cfg", config);
                    files.push_back(folder / L"grub.cfg");
                }
            } else {
                std::filesystem::create_directories(mount / L"diskboot");
                image.copy_iso(mount / L"diskboot" / L"installer.iso");
                require(hash_file(mount / L"diskboot" / L"installer.iso") == image.metadata.sha256,
                        "Staged ISO hash mismatch");
                before_write();
                const auto marker = std::filesystem::path(L"diskboot") / (image.metadata.sha256 + ".marker");
                write_file(mount / marker, "DiskBoot laboratory recovery media\n");
                files.push_back(L"diskboot/installer.iso");
                files.push_back(marker);
                durable_json(mount / L"diskboot" / L"plan.json", Json(approved), true);
                files.push_back(L"diskboot/plan.json");
            }
            for (const auto &file : files) {
                before_write();
                state.journal["files"].push_back({{"partition_guid", partition.guid},
                                                  {"volume", partition.volume_path},
                                                  {"relative", utf8(file.wstring())},
                                                  {"sha256", hash_file(mount / file)}});
            }
            checkpoint("boot-files-verified");
            before_write();
            checkpoint("unmount-intent");
            backend.storage().unmount(partition, mount);
            wchar_t removed[1024]{};
            auto    root = mount.wstring() + L"\\";
            require(!GetVolumeNameForVolumeMountPointW(root.c_str(), removed, 1024),
                    "Mount removal could not be verified");
            std::filesystem::remove(mount);
            state.journal.erase("mount");
            checkpoint("unmount-verified");
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
} // namespace diskboot
