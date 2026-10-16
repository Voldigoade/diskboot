---
name: diskboot-systems-safety
description: Safety guidance for this Windows C++ project preparing internal UEFI boot media for Linux installers. Use for disk enumeration, GPT partitioning, NTFS resizing, UEFI, EFI System Partitions (ESP), Secure Boot, Windows Storage Management API, MSFT_Disk, MSFT_Partition, ISO boot, GRUB, shim, Ubuntu/Kubuntu boot, openSUSE boot, and VHD/VHDX integration tests.
---

# Diskboot Systems Safety

## Establish evidence

- Prefer authoritative Microsoft Learn documentation through the `microsoft-learn` MCP for Windows APIs, including storage classes, method contracts, limits, and return values. If unavailable, use official Microsoft Learn pages and disclose the fallback.
- Prefer official Ubuntu/Kubuntu and openSUSE documentation for their boot behavior. Confirm applicability to the exact installer release, ISO, architecture, and boot chain being used.
- Never guess disk identities, partition offsets, boot behavior, or Secure Boot behavior. Resolve uncertainty through authoritative documentation and observed state; otherwise stop the dependent operation and report what evidence is missing.

## Guard storage mutations

- Fail closed when disk identity or layout is ambiguous. Identify the target from freshly queried, corroborating identifiers and layout; never rely solely on a disk number, drive letter, enumeration order, or cached state. Revalidate immediately before writing.
- Treat the running Windows system disk and its ESP as protected by default. Positively identify them and enforce protection in target selection and mutation paths; uncertainty is a reason to refuse the mutation.
- Do not modify BCD or the Windows ESP unless the task explicitly requires a separately reviewed compatibility mode. General boot-media preparation does not authorize that mode.
- Before partitioning or NTFS resizing, query the actual layout and supported resize bounds; derive offsets, sizes, alignment, and non-overlap from verified state and documented constraints.
- After every mutation, re-query actual disk, partition, volume, and filesystem state as applicable, and compare against the intended result before continuing. A successful API return alone is insufficient. On mismatch or unverifiable state, stop further writes and report the observed state; do not retry blindly.

## Test and report

- Never run destructive storage tests against real physical disks. Use VHD/VHDX or disposable VMs, and verify that every destructive test target is the intended disposable virtual disk; a VM alone does not make a passed-through physical disk safe.
- Test boot and Secure Boot claims in a disposable UEFI VM with recorded firmware settings, trust configuration, ISO version, and bootloader versions. Do not infer successful boot or signature acceptance from file presence, partition creation, or another distribution's behavior.
- Distinguish observed and tested results from documentation-based expectations and untested inferences. Record the test environment, before/after storage state, and remaining verification gaps.
