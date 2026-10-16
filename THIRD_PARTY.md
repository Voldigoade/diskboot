# Third-party material

DiskBoot is distributed under GPLv3; see [LICENSE](LICENSE).

| Material | Version / origin | License | Current use |
| --- | --- | --- | --- |
| BootThatISO | Commit in [UPSTREAM.md](UPSTREAM.md) | GPLv3 | Fork history and provenance |
| nlohmann/json | 3.12.0, [official source](https://github.com/nlohmann/json/tree/v3.12.0) | [MIT](third_party/nlohmann/LICENSE.MIT) | Plan, inventory, and journal serialization |
| 7-Zip | Upstream vendored SDK | LGPL with file-specific exceptions | No SDK code or binaries retained or linked; [historical notices](third_party/7zip-notices/License.txt) retained |

The vendored `third_party/nlohmann/json.hpp` has SHA-256 `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`. Its embedded copyright and license text are unmodified.

EFI binaries are never bundled by DiskBoot. Laboratory preparation extracts only specific boot components from the user's ISO; their distribution licensing continues to apply. No executable from an ISO is executed by the Windows application.
