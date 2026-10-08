# AnyPS5

Convert guest x86-64 executables into native Linux or Windows programs.
AnyPS5 combines a [relinker](core/relinker) with native
[system library implementations](core/libs/prx) and an RDNA-to-SPIR-V shader
recompiler. Converted applications run as normal host processes and dynamically
link the supplied libraries.

This is an early compatibility project. Unsupported states fail explicitly;
conversion success and implementation percentages do not prove that a title is
playable. See [recorded compatibility](docs/user/COMPATIBILITY.md).

## Start here

| Goal | Read |
| --- | --- |
| Convert and run an application | [Getting started](docs/user/GETTING_STARTED.md) |
| Build the project | [Build instructions](docs/dev/BUILD.md) |
| Make a first contribution | [Development workflow](docs/dev/DEVELOPMENT.md) and [Contributing](CONTRIBUTING.md) |
| Understand the code | [Repository map](docs/dev/REPOSITORY_MAP.md) and [Architecture](docs/dev/ARCHITECTURE.md) |
| Diagnose a failure | [Troubleshooting](docs/user/TROUBLESHOOTING.md) |
| Work with a coding agent | [AGENTS.md](AGENTS.md) |

The [documentation index](docs/README.md) links every maintained guide.

## Quick start: conversion tool

From a Git checkout with CMake 3.22.1+, Ninja, a C++20 compiler and Python 3:

```sh
cmake --preset relinker
cmake --build --preset relinker
ctest --preset relinker
```

This dependency-free build works on Linux, Windows and macOS, including Apple
Silicon. Output is still x86-64 Linux ELF or Windows PE. It does not supply
system libraries or macOS game execution. See the
[full build](docs/dev/BUILD.md#full-build) for native libraries and graphics tests.

```sh
./build-relinker/core/relinker/relinker source/input.elf app.elf
./build-relinker/core/relinker/relinker --windows source/input.elf app.exe
```

Prepare input module directories and runtime resources as described in
[Getting started](docs/user/GETTING_STARTED.md). Windows uses `relinker.exe`;
add `--to-intel` for an Intel runtime host. The `libs` target must be built
separately for a full runtime.

## Project layout

```text
core/relinker/          ELF parsing, CPU lowering, native ELF/PE output
core/libs/prx/          Native system APIs and Vulkan graphics driver
core/libs/nid/          Export-name patching to guest NIDs
core/shader/recompiler/ RDNA decoding, IR, optimization, SPIR-V output
core/Decoder/           JPEG and PNG decoders
3rdparty/               Pinned dependency submodules
tools/                  Documentation/conventions checks, progress, releases, oracle
docs/                   User and developer guides
.github/               CI, release workflows and contribution templates
```

See the [repository map](docs/dev/REPOSITORY_MAP.md) for entry points and tests.

## Status and controls

[![libraries](https://boykopovar.github.io/AnyPS5/badge-libraries.svg)](https://boykopovar.github.io/AnyPS5/)
[![shaders](https://boykopovar.github.io/AnyPS5/badge-shaders.svg)](https://boykopovar.github.io/AnyPS5/)

[![progress map](https://boykopovar.github.io/AnyPS5/progress.svg)](https://boykopovar.github.io/AnyPS5/)

These upstream progress reports count functions/instructions known to the
project, not all system APIs, verified behavior or playable titles. See
[Progress reporting](docs/dev/PROGRESS.md) for their interpretation and
[Technical debt](docs/dev/TechnicalDebt.md) for known gaps.

SDL-mapped controllers and configurable keyboard/mouse input are supported.
See [Input mapping](docs/user/INPUT_MAPPING.md). Runtime paths, fonts, GPU
selection and all CLI options are documented in [Usage](docs/user/USAGE.md).

## Contributing

Follow [Contributing](CONTRIBUTING.md) for implementation and review rules.
Use [Testing](docs/dev/TESTING.md) to select checks. Maintained agent guidance
lives in the root and subsystem `AGENTS.md` files. Bug reports should include a
minimal reproduction, the first failure and host/build details; use the
[bug report form](.github/ISSUE_TEMPLATE/bug_report.yml).

## Disclaimer

This project is intended for interoperability, research, preservation, and
compatibility purposes. It does not include, distribute, or require copyrighted
software, firmware, cryptographic keys, or proprietary libraries. Users are
responsible for ensuring that any binaries used with this project are obtained
and used in accordance with applicable laws and their respective license terms.

## License

[GNU General Public License version 2 only](LICENSE).
