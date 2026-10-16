# Final project status

Development stopped on 2026-10-16. This is an experimental reference prototype with no production support or validated installation workflow.

## Preserved implementation

- Shared C++20 engine for the native Windows GUI and CLI.
- Corroborated disk inventory, Windows boot-chain protection, stable identity checks, allocation planning and NTFS shrink-bound preflight.
- Bounded ISO9660 inspection, SHA-256 verification, candidate Ubuntu/Kubuntu recognition and explicit openSUSE refusal.
- Journaled storage execution with intent recording and actual-state verification after each mutation. The executable preparation backend accepts explicit disposable VHD/VHDX targets only; physical preparation is disabled.
- Candidate EFI/kernel/initrd staging, file manifests, synthetic tests, optional laboratory harnesses and Windows CI configuration.

## Last tested state

The final source snapshot is `bfb021cd4e05207d34a2053564c1ae2645a89f1c`. Closure changed documentation and removed temporary resources; it introduced no new features or storage/boot tests.

| Check | Observed result |
| --- | --- |
| Windows x64 Debug and Release builds | Passed with Visual Studio 2022, MSVC 19.44 and Windows SDK 10.0.26100; warnings treated as errors |
| Default CTest suites, both configurations | 3/3 passed: 26 core checks, 9 transaction checks, 11 ISO cases and 1 physical-execution refusal; 47 checks total |
| Formatting | Passed with clang-format 22.1.3 |
| Additional static analysis | No separate static-analysis target was configured or run |
| Native GUI | Earlier startup smoke check passed; visual, accessibility, DPI and full workflow QA remain incomplete. Final review-panel edits were compiled, not interactively validated |
| Host inventory | Read-only observation identified and protected separate Windows OS and ESP disks. No real physical disk mutations were tested |

The following laboratory observations predate discontinuation and were not repeated during closure:

- Disposable VHDX storage tests passed native NTFS shrinking, partition creation, FAT32/NTFS formatting, exact layout checks and preservation of a data sentinel.
- Final media preparation used an 80 GiB disposable VHDX with a 64 GiB NTFS source, a 40 GiB total allocation and a new 1 GiB FAT32 ESP. The source shrank to 24 GiB. The `ubuntu-casper-staged-kernel` profile reached `laboratory-media-verified`, with ten staged-file manifest entries and `boot_tested=false`.
- The inspected ISO was `kubuntu-26.04.1-desktop-amd64.iso`, 5,072,990,208 bytes, SHA-256 `831e4d4bb85098339ba43d3502cd6619b27e76daf37246a084cd68a6413090b8`, matching the distribution's published checksum. Its metadata identified the amd64 20260826.1 image.
- An earlier loopback profile reached GRUB 2.14 in a Generation 2 Hyper-V VM with 12 GiB RAM and guest Secure Boot off. GRUB could not read the NTFS recovery filesystem; loading its missing NTFS module also failed. No Linux kernel, live environment or installer was observed. The final staged-kernel profile was never boot-tested.

## Unfinished work and risks

Successful installer boot, release of installation media before changing the same disk, same-disk installation, Secure Boot trust/revocation behavior and broad physical hardware support remain unverified. openSUSE has no executable adapter. Interrupted real storage operations, recovery, automatic rollback/resume, a live-installer disk guard and complete GUI validation remain unfinished.

Storage-provider success and verified file hashes do not establish a bootable or safe installation workflow. The third-party installer can modify disks outside the preparation plan. The RAM preflight is a conservative policy, not a measured minimum. The upstream audit was architectural, not a complete security certification.

## Closure

The task-created VM, its checkpoint/copy, nine retained VHDX test targets, temporary audit clone, build output, downloaded ISO and debugging scratch files were removed. No task-created virtual disk, mount, temporary drive letter or helper remained active after cleanup. Five inactive laboratory journals were summarized above and deleted with explicit owner approval. No host physical disk, BCD, existing ESP or firmware setting was changed during cleanup.

Useful source, tests, logical development commits, GPLv3, upstream history and third-party notices are retained. No stable DiskBoot release was created.
