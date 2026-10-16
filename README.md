# DiskBoot

**Experimental discontinued prototype. Active development has stopped. This project is not production-ready.** It manipulates storage and boot concepts and should not be used on machines containing important data. Destructive disk paths were not fully validated across physical hardware. The repository is preserved for reference and possible future work.

DiskBoot attempted to prepare Linux installer media on a secondary internal GPT disk from Windows, without a USB drive, while preserving existing NTFS data and protecting the Windows boot chain.

## Implemented scope

The Windows C++20 prototype includes a shared native GUI/CLI engine, disk inventory and protection checks, deterministic allocation planning, bounded ISO inspection and SHA-256 hashing, transaction journals, and a disposable VHD/VHDX preparation backend. Physical-disk preparation is disabled. The candidate Ubuntu/Kubuntu adapter stages EFI files, a kernel and an initrd alongside an ISO on separate recovery media. openSUSE remains unsupported.

## Validation and limitations

Safe synthetic tests cover planning, refusal paths, transaction failures and ISO parsing. Earlier disposable VHDX tests exercised NTFS shrinking, new partitions, file verification and preservation of a data sentinel. One UEFI VM reached GRUB but failed to boot the ISO. The subsequent staged-kernel adapter was prepared on a VHDX but was never boot-validated.

Successful Linux installer boot, same-disk installation, source release, Secure Boot, broad physical hardware compatibility and recovery from interrupted real storage operations remain unverified. No live-installer disk guard or automatic rollback/resume exists. See [STATUS.md](STATUS.md) for the exact last tested state.

## Build

Requires Windows x64, Visual Studio 2022 with the C++ desktop workload and Windows SDK, CMake 3.25 or newer, and Python 3.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DDISKBOOT_VHD_TESTS=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The default tests use synthetic data and temporary ordinary files. Destructive laboratory harnesses remain as reference source and are excluded from the default test suite.

## Attribution and license

Forked from [BootThatISO](https://github.com/tiirwaa/BootThatISO-WITHOUT-USB---NO-USB-NEEDED-), with original history, attribution and legal notices retained. Distributed under [GPLv3](LICENSE). See [upstream provenance](UPSTREAM.md), [audit scope](docs/upstream-audit.md) and [third-party notices](THIRD_PARTY.md).
