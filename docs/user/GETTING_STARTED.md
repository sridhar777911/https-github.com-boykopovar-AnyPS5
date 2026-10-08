# Getting started

AnyPS5 converts a clean guest x86-64 ELF and its bundled modules into native
Linux or Windows binaries. Native system libraries supply the guest APIs, and
the graphics path translates RDNA shaders to SPIR-V for Vulkan.

## Choose a build

| Goal | Build | Host |
| --- | --- | --- |
| Work on conversion or inspect output | Relinker only | Linux, Windows, macOS (including Apple Silicon) |
| Build native system libraries and run a title | Full | x86-64 Linux or supported MinGW-w64 Windows |
| Work on shaders or graphics | Full with relevant tests | Linux/Windows with suitable Vulkan support |

Relinker-only builds do not provide runtime libraries or macOS game execution.
See [Build](../dev/BUILD.md) for exact toolchains. A source archive does not
contain Git submodule contents; use a Git clone for a full build.

## Build the conversion tool

From the repository root, with CMake 3.22.1+, Ninja, a C++20 compiler and Python 3:

```sh
cmake --preset relinker
cmake --build --preset relinker
ctest --preset relinker
```

The executable is `build-relinker/core/relinker/relinker`, or `relinker.exe` on
Windows. Python enables the synthetic-binary regression tests.

## Prepare input

Use a clean ELF you are authorized to use. Keep its bundled ELF modules beside
it in `sce_module/`, `sce_modules/`, or `prx/`. `prx/` can coexist with one of
the other directories, but `sce_module/` and `sce_modules/` cannot coexist.
If there are no bundled modules, create an empty `sce_module/` directory.
The normal conversion path requires at least one recognized directory.

```text
source/
  input.elf
  sce_module/
```

## Convert

Create the output directory first. On a Unix shell:

```sh
mkdir -p runtime
./build-relinker/core/relinker/relinker source/input.elf runtime/app.elf
```

For Windows output from a Unix host:

```sh
./build-relinker/core/relinker/relinker --windows source/input.elf runtime/app.exe
```

From Windows PowerShell:

```powershell
New-Item -ItemType Directory -Force runtime
.\build-relinker\core\relinker\relinker.exe --windows source\input.elf runtime\app.exe
```

Add `--to-intel` for an Intel runtime host. The output format defaults to Linux;
the filename extension does not choose Windows. Successful conversion alone
does not establish title compatibility.

## Prepare execution

Build the full project for the target OS, including `cmake --build build
--target libs`. Place the patched libraries from `build/core/libs/libs/` in
`runtime/libs/`. Copy application resources separately into `runtime/app0/`,
preserving converted bundled modules already emitted there.

```text
runtime/
  app.elf or app.exe
  libs/
    <patched system libraries>.prx
  app0/
    <application resources>
    sce_module/
      <converted bundled modules>
```

Use a compatible x86-64 Linux/Windows host and Vulkan device. Run `chmod +x
runtime/app.elf` then `./runtime/app.elf` on Linux, or `.\runtime\app.exe` in
PowerShell. Conversion on an Apple Silicon Mac still produces x86-64 output.

Read [Usage](USAGE.md) for library search paths, fonts, GPU selection and Windows
memory requirements. Check [Compatibility](COMPATIBILITY.md) before expecting a
title to run. Use [Troubleshooting](TROUBLESHOOTING.md) for the first failure.
