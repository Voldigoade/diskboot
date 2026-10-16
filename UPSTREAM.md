# Provenance

DiskBoot is a GPLv3 fork of [BootThatISO](https://github.com/tiirwaa/BootThatISO-WITHOUT-USB---NO-USB-NEEDED-), starting at commit `70a5691e089827f209892629520594bd163101c4`. Original history and the GPL license are preserved. The `upstream` remote points to that repository. Upstream authors retain their copyrights.

The old application, embedded GRUB, and vendored archive implementation have been removed from the current tree. DiskBoot's planner, Windows backend, ISO reader, executor, and interfaces are new implementations. Historical source and attribution remain accessible through Git history.

[ULLI](https://github.com/rltvty2/ulli) at `48a28feb63be934185c0b1c6ed2462bc6e04d723` was inspected for boot-flow and compatibility ideas. No ULLI source was copied. Its BCD/default-firmware changes and broad boot assumptions were not adopted.

See [the audit](docs/upstream-audit.md) and [third-party notices](THIRD_PARTY.md).
