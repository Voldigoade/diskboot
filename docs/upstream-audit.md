# Upstream disposition

Base: `70a5691e089827f209892629520594bd163101c4` (1,414 tracked files).

Every tracked path was inventoried before cleanup. This was an architectural and entry-point review, not a line-by-line security certification of the 7-Zip SDK. All original executable paths and vendored code were removed, so an unreviewed SDK component cannot remain reachable through the new build.

| Paths | Finding and disposition |
| --- | --- |
| `src/models/DiskpartExecutor.cpp`, `PartitionCreator.cpp`, `SpaceManager.cpp`, `src/services/partitionmanager.cpp` | Fixed Disk 0 / C selection, fixed allocation and recovery behavior. Removed; native provider plus deterministic range planning replaces them. |
| `src/services/BCD*`, `bcdmanager.*`, `src/models/LinuxBootStrategy.h`, EFI managers | BCD changes, boot defaults, embedded GRUB and fixed `(hd0,gpt3)` ISO path. Removed. |
| `src/controllers`, `src/views`, `src/main.cpp` | Old UI/controller flow reaches the unsafe storage and BCD services. Removed as a unit. |
| ISO/archive, hash, file-copy and utility modules | Shell helpers and extraction surface are coupled to the old application. Removed; bounded read-only ISO9660 inspection and Windows SHA-256 replace them. |
| WIM, drivers, Windows/Hiren configuration and integration modules | Outside the Linux secondary-disk scope. Removed. |
| `include`, `chainloader`, `res`, `lang`, screenshots, scripts, old tests and tools | No remaining callers after replacing the application. Removed, including embedded binaries and obsolete translations. |
| `third-party` | Original build selected ISO/UDF handlers from a much larger SDK tree. No subset is retained or linked. License, LGPL text and unRAR notice preserved under `third_party/7zip-notices`. |
| README translations, architecture/protection documents, release notes and `.github` | Describe the old product or publish old binaries. Removed; documentation and CI describe only the replacement. |
| `LICENSE`, `.clang-format`, `.gitignore`, `CMakeLists.txt` | GPL retained verbatim. Build and ignore rules replaced; formatting configuration retained. |

ULLI's Windows script was inspected at its recorded commit. It creates BCD firmware entries and changes firmware display/default order. Its extracted-media approach is useful research context, but does not validate DiskBoot's loopback profile, source release, or installer behavior. Neither its code nor security-setting advice is included.
