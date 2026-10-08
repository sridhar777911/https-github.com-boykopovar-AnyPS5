# Repository map

| Path | Responsibility | First place to inspect |
| --- | --- | --- |
| `CMakeLists.txt` | Full/relinker-only build selection and common switches | [Build](BUILD.md) |
| `CMakePresets.json` | Repeatable configure/build/test commands | [Development](DEVELOPMENT.md) |
| `core/relinker/` | Guest binary conversion | [main.cpp](../../core/relinker/main.cpp) |
| `core/relinker/cli/` | Arguments and autorun | [CliArgs.cpp](../../core/relinker/cli/src/CliArgs.cpp) |
| `core/relinker/domain/` | Shared conversion data and runtime contracts | [Types.hpp](../../core/relinker/domain/include/domain/Types.hpp) |
| `core/relinker/io/` | Bounded byte and file I/O | [ByteReader.cpp](../../core/relinker/io/src/ByteReader.cpp) |
| `core/relinker/codegen/` | AMD-only CPU instruction lowering | [Amd64OnlyConverter.cpp](../../core/relinker/codegen/src/Amd64OnlyConverter.cpp) |
| `core/relinker/relinker/` | ELF parsing, analysis, imports and guest modules | [RelinkerPipeline.cpp](../../core/relinker/relinker/src/pipeline/RelinkerPipeline.cpp) |
| `core/relinker/elfpatcher/` | Native ELF/PE construction | [LinuxElfPatcher.cpp](../../core/relinker/elfpatcher/src/linux/LinuxElfPatcher.cpp), [WindowsPePatcher.cpp](../../core/relinker/elfpatcher/src/windows/WindowsPePatcher.cpp) |
| `core/libs/` | Library builds, export patching and shared infrastructure | [CMakeLists.txt](../../core/libs/CMakeLists.txt) |
| `core/libs/nid/` | Export-name to NID conversion | [NidCompute.cpp](../../core/libs/nid/src/NidCompute.cpp) |
| `core/libs/prx/` | Native implementations of system APIs | [Library guidance](../../core/libs/AGENTS.md) |
| `core/libs/prx/libSceAgcDriver/` | Command submission, PM4, Vulkan graphics | [Driver guidance](../../core/libs/prx/libSceAgcDriver/AGENTS.md) |
| `core/shader/recompiler/` | RDNA decoding, IR, optimization and SPIR-V | [Recompiler.cpp](../../core/shader/recompiler/Recompiler.cpp) |
| `core/Decoder/` | JPEG/PNG support and tests | [JPEG](../../core/Decoder/Jpeg/CMakeLists.txt), [PNG](../../core/Decoder/Png/CMakeLists.txt) |
| `3rdparty/` | Pinned dependency submodules | [.gitmodules](../../.gitmodules) |
| `tools/` | Checks, progress, NID names and release packaging | [Maintenance](MAINTENANCE.md) |
| `tools/hw-oracle/` | Local AMD GPU instruction measurement | [Hardware oracle](HW_ORACLE.md) |
| `.github/` | Build/release workflows and contribution templates | [Maintenance](MAINTENANCE.md) |
| `docs/` | Maintained user/developer documentation | [Index](../README.md) |

## Finding tests

Relinker C++ tests live under the owning component's `tests/` directory;
synthetic ELF/PE fixtures live in `core/relinker/relinker/tests/`. Most shader
and Vulkan tests are registered in `libSceAgcDriver/CMakeLists.txt`, including
`tests/execution/`. Decoder tests live beside each decoder; Python tooling tests
live in `tools/tests/`. [Testing](TESTING.md) explains discovery and selection.

## Generated output

`build*/` contains generated build state. Patched runtime libraries are under
`build/core/libs/libs/`; `unpatched/` contains intermediates. Progress output,
release archives, shader caches and converted applications are generated
artifacts, not implementation source. Keep them outside tracked files.
