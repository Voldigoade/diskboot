#include "platform/windows/virtual_backend.hpp"
#include <iostream>

using namespace diskboot;
int wmain(int argc, wchar_t **argv) {
    try {
        require(argc == 4, "Usage: vhd_driver <attached-vhdx> <source-partition-guid> <journal-path>");
        ComRuntime     runtime;
        VirtualBackend backend(argv[1]);
        const auto     inventory = backend.inventory(true);
        const Disk    *target    = nullptr;
        for (const auto &disk : inventory.disks) {
            if (disk.identity.bus != 14 && disk.identity.bus != 15)
                continue;
            try {
                backend.authorize(disk.identity);
            } catch (const Error &) {
                continue;
            }
            require(!target, "Virtual target ambiguous");
            target = &disk;
        }
        require(target, "Supplied disposable VHD could not be positively identified");
        Iso image;
        image.path      = "storage-test-only";
        image.sha256    = std::string(64, 'a');
        image.size      = 100 * MiB;
        image.profile   = "ubuntu-casper-loopback";
        image.candidate = true;
        auto plan       = make_plan(inventory, target->identity, image, 16 * GiB, lower(utf8(argv[2])), true);
        bool first      = true;
        auto result     = execute_partitions(plan, backend, [&](const Json &value) {
            durable_json(argv[3], value, first);
            first = false;
        });
        revalidate(backend, plan.target, result.expected);
        std::cout << Json{{"partition_operations_verified", true},
                          {"boot_files_written", false},
                          {"plan", plan},
                          {"actual", result.expected}}
                         .dump(2)
                  << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
