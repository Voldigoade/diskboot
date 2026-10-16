# Implementation evidence

Windows API contracts were retrieved through the Microsoft Learn MCP during implementation. These are the relevant maintained references, not claims of end-to-end boot validation.

| Decision | Authoritative reference |
| --- | --- |
| Stable identity, transient disk numbers and boot/system flags | [MSFT_Disk](https://learn.microsoft.com/windows-hardware/drivers/storage/msft-disk), [MSFT_Partition](https://learn.microsoft.com/windows-hardware/drivers/storage/msft-partition) |
| Supported shrink range and immovable files | [GetSupportedSize](https://learn.microsoft.com/windows-hardware/drivers/storage/msft-partition-getsupportedsizes), [Resize](https://learn.microsoft.com/windows-hardware/drivers/storage/msft-partition-resize) |
| Explicit creation ranges and provider error semantics | [CreatePartition](https://learn.microsoft.com/windows-hardware/drivers/storage/createpartition-msft-disk), [Format](https://learn.microsoft.com/windows-hardware/drivers/storage/format-msft-volume) |
| Provider binding and return-value checking | [Calling a provider method](https://learn.microsoft.com/windows/win32/wmisdk/example--calling-a-provider-method) |
| Independent layout corroboration | [IOCTL_DISK_GET_DRIVE_LAYOUT_EX](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntdddisk/ni-ntdddisk-ioctl_disk_get_drive_layout_ex) |
| VHD device mapping; permanently attached handle access | [GetVirtualDiskPhysicalPath](https://learn.microsoft.com/windows/win32/api/virtdisk/nf-virtdisk-getvirtualdiskphysicalpath), [OpenVirtualDisk](https://learn.microsoft.com/windows/win32/api/virtdisk/nf-virtdisk-openvirtualdisk) |
| Actual firmware variables and privilege requirements | [GetFirmwareEnvironmentVariableW](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-getfirmwareenvironmentvariablew) |

## Candidate Ubuntu/Kubuntu path

[Ubuntu's ISO boot guidance](https://help.ubuntu.com/community/Grub2/ISOBoot) and the [Casper manual](https://manpages.ubuntu.com/manpages/noble/man7/casper.7.html) describe loopback and RAM-backed live media. The candidate uses the exact ISO's `casper/vmlinuz`, `casper/initrd`, and EFI files, a hash-specific recovery marker, and a fixed destination ISO name. It does not use firmware disk ordering or a fixed GPT partition number.

The RAM preflight is a conservative policy of ISO size plus half its size plus 4 GiB. It is not a measured minimum. `toram` alone does not prove that `/isodevice` has been released or that the installer can reread a modified GPT. Those transitions must be observed in the exact ISO's live environment.

[Ubuntu's Secure Boot chain](https://documentation.ubuntu.com/security/security-features/platform-protections/secure-boot/) requires appropriate firmware trust, shim, GRUB and kernel validation. Copying the distribution's files does not establish trust, revocation status, GRUB prefix resolution, or available loopback/filesystem modules. The laboratory profile refuses enabled or unknown host Secure Boot state. No security setting is changed by the application.

## openSUSE

[Leap boot parameters](https://doc.opensuse.org/documentation/leap/startup/html/book-startup/cha-boot-parameters.html) describe installer source selection. [Tumbleweed media guidance](https://doc.opensuse.org/documentation/tumbleweed/image_choice/) describes installation images. These do not establish that DiskBoot's same-disk recovery layout is usable during installation. The reader recognizes the loader layout for a precise refusal, but supplies no Ubuntu arguments and no executable openSUSE profile. Leap and Tumbleweed remain unsupported pending independent source-release and Secure Boot tests.

## Mutation boundary

All new partitions and staged files belong to the plan's allocation. Only the chosen original NTFS partition may shrink. Windows APIs necessarily update filesystem metadata within that original partition and GPT metadata elsewhere on the same target disk. A claim that every physical write is inside the newly allocated extent would be false. DiskBoot performs no raw writes and does not claim a storage-provider transaction can be rolled back atomically.

Intent is flushed before each operation, and the observed layout is checked before continuing. Incomplete journals stop new work on that identity. Automatic rollback or resumption after uncertain side effects is deliberately unavailable; inspect the recorded original layout, last intent, and actual disk before recovery.
