#include "app/service.hpp"
#include <iostream>
#include <map>

using namespace diskboot;
int wmain(int argc, wchar_t **argv) {
    try {
        ComRuntime runtime;
        require(argc >= 2,
                "Usage: diskboot disks | inspect <iso> | plan --iso <path> --disk <id> --size 500GB [--partition "
                "<guid>] | prepare ... --lab-vhd <path> [--confirm <id>] | status | verify <journal>");
        const std::wstring command = argv[1];
        if (command == L"disks") {
            require(argc == 2, "disks takes no arguments");
            std::cout << disk_inventory().dump(2) << '\n';
        } else if (command == L"inspect") {
            require(argc == 3, "inspect requires one ISO path");
            IsoImage image(argv[2]);
            std::cout << Json(image.metadata).dump(2) << '\n';
        } else if (command == L"status") {
            require(argc == 2, "status takes no arguments");
            std::cout << transaction_status().dump(2) << '\n';
        } else if (command == L"verify") {
            require(argc == 3, "verify requires a transaction journal");
            std::cout << verify_transaction(argv[2]).dump(2) << '\n';
        } else if (command == L"plan" || command == L"prepare") {
            std::map<std::string, std::string> options;
            for (int i = 2; i < argc; i += 2) {
                require(i + 1 < argc, "Missing option value");
                const auto key = utf8(argv[i]);
                require(key == "--iso" || key == "--disk" || key == "--size" || key == "--partition" ||
                            key == "--lab-vhd" || key == "--confirm" || key == "--guest-secure-boot" ||
                            key == "--guest-ram",
                        "Unknown option");
                require(options.emplace(key, utf8(argv[i + 1])).second, "Duplicate option");
            }
            require(options.contains("--iso") && options.contains("--disk") && options.contains("--size"),
                    "ISO, disk ID and total size required");
            Request request{wide(options["--iso"]), wide(options["--lab-vhd"]), options["--disk"],
                            options["--partition"], parse_size(options["--size"])};
            if (options.contains("--guest-secure-boot") || options.contains("--guest-ram")) {
                require(!request.lab_vhd.empty() && options.contains("--guest-secure-boot") &&
                            options.contains("--guest-ram"),
                        "Guest settings require a disposable VHD and both guest Secure Boot state and RAM");
                request.guest_secure_boot = options["--guest-secure-boot"];
                request.guest_ram         = parse_size(options["--guest-ram"]);
            }
            if (command == L"prepare")
                require(!request.lab_vhd.empty(),
                        "Physical preparation is disabled: boot profiles have not passed UEFI installer validation");
            const auto plan = plan_request(request);
            if (command == L"plan")
                std::cout << Json{{"dry_run", true},
                                  {"plan", plan},
                                  {"summary", summarize(plan)},
                                  {"physical_preparation_enabled", false}}
                                 .dump(2)
                          << '\n';
            else {
                std::cout << summarize(plan);
                if (options.contains("--confirm"))
                    require(options["--confirm"] == stable_id(plan.target),
                            "Confirmation does not match target identity");
                else {
                    std::cout << "Modify this disposable virtual disk? Type yes: ";
                    std::string answer;
                    std::getline(std::cin, answer);
                    require(answer == "yes", "Preparation cancelled");
                }
                auto journal = prepare_virtual(request, plan, [](const auto &phase) { std::cerr << phase << '\n'; });
                std::cout << "Laboratory media prepared; boot remains unverified. Journal: " << utf8(journal.wstring())
                          << '\n';
            }
        } else
            throw Error("Unknown command");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "DiskBoot: " << e.what() << '\n';
        return 1;
    }
}
