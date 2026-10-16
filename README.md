# DiskBoot

Windows C++20 tooling for preparing internal Linux installer media on a secondary GPT disk without a USB drive.

**Development status:** the planner, read-only inventory and ISO inspection are implemented. Boot profiles have not passed a UEFI installer test. Physical-disk preparation is disabled. This is not a usable Linux installation release yet.

Windows 10/11 x64 and UEFI are the intended hosts. SATA/NVMe, GPT and 512-byte logical sectors are the initial scope. Windows boot-chain disks, existing ESPs and recovery partitions are protected. Ambiguous evidence stops planning or execution. DiskBoot does not modify BCD, firmware boot variables or an existing ESP.

Allocations are total budgets: boot, recovery and remaining Linux space are included. The planner uses existing unallocated space first and only proposes shrinking a selected healthy NTFS partition within Windows-reported bounds. It does not delete or format existing partitions.

## Build and tests

From a Visual Studio 2022 installation with the C++ desktop workload and Windows SDK:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Unit tests use synthetic disk inventories and never modify storage. Destructive integration work must use disposable VHD/VHDX files or VMs. The Windows backend requires sufficient permissions for storage inventory; missing evidence is never replaced with assumed disk identities.

Ubuntu/Kubuntu Casper layouts are candidates, not supported installer profiles. openSUSE Leap and Tumbleweed remain unsupported until their source discovery and same-disk installer behavior are independently validated. Signed EFI files alone do not establish Secure Boot compatibility.

DiskBoot preparation protection does not constrain a third-party Linux installer. No live-environment disk guard is implemented.

GPLv3. See [provenance](UPSTREAM.md), [audit scope](docs/upstream-audit.md), and [third-party notices](THIRD_PARTY.md).
