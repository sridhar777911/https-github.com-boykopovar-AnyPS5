# Troubleshooting

Start with the first error. Keep the exact command, commit, host/toolchain and
stderr; separate configure, build, conversion, loader and runtime failures.

## Build failures

| Symptom | Check and action |
| --- | --- |
| Missing `3rdparty/*/CMakeLists.txt` | Use a Git clone and `git submodule update --init --recursive` for full builds. Use relinker-only mode for conversion work. |
| Full build fails on macOS | Full runtime targets Linux/Windows x86-64. Use the relinker preset on macOS. |
| CMake cannot find Ninja/compiler | Put the required build tools on `PATH`; Windows full builds use the pinned WinLibs toolchain in [Build](../dev/BUILD.md). |
| Generator/compiler mismatch in cache | Configure a fresh build directory when changing generators, compilers or build modes. |
| SDL cannot find X11/Xext headers | Install the development headers listed in the build guide on Linux. |
| Windows FFmpeg download returns status 60 | Set `SSL_CERT_FILE` to a valid CA bundle as documented in the build guide. |
| No Python regression tests | Install Python 3 before configuring and reconfigure; inspect `ctest --test-dir <build> -N`. |
| Library edits have no runtime effect | Build `libs`, then refresh the runtime's patched `.prx` files. |

## Conversion failures

- Input must be a clean ELF, not a package, encrypted container or original
  module placed in the host library directory.
- Use at least one recognized module directory: `sce_module/`, `sce_modules/`,
  or `prx/`. Do not provide both `sce_module/` and `sce_modules/`.
- Use `--windows` for PE output. A filename ending in `.exe` still selects ELF
  without the flag.
- Quote `$ORIGIN` in a custom `--rpath` so the shell does not expand it.
- `unused-filter=0|1|2` has no leading `--` and appears at most once.
- Conversion exit code `1` means invalid arguments; `2` means conversion
  failed. With `--autorun`, a converted program's exit code is returned.
- Deprecated skip/check/lazy-binding flags weaken validation and are only
  debugging aids. They do not establish a working runtime.

Read [Usage](USAGE.md) for the full option and input contract.

## Startup and runtime failures

| Symptom | Check and action |
| --- | --- |
| Missing import/NID/library | Confirm target OS, patched system libraries, converted bundled modules and search path. Unsupported exports intentionally fail. |
| Windows error 193 loading a `.prx` | Original guest modules cannot load as native DLLs. Keep converted guest modules under `app0/` and patched system libraries under `libs/`. |
| Windows direct-memory backing error | Check the system commit limit (RAM plus page file) against the allocation described in [Usage](USAGE.md#runtime-layout). |
| No suitable Vulkan device | Inspect `Physical device candidate` lines; select a reported device with `ANYPS5_GPU` if needed. |
| Missing system-font text | Supply font files/substitutes in `anyps5-fonts/` or set `ANYPS5_SYSTEM_FONTS`; see [system fonts](USAGE.md#system-fonts). |
| Input initialization error | Check the reported line in `anyps5-input.ini` or `ANYPS5_INPUT_CONFIG`; see [input mapping](INPUT_MAPPING.md). |
| Suspected stale shader output | Compare a run with `ANYPS5_NO_SHADER_CACHE=1`; the default cache is `shader_cache/` beside the executable. `ANYPS5_SHADER_CACHE_DIR` overrides its location. |
| `std::runtime_error` for unsupported behavior | Preserve the full message and minimal reproduction. Strict failures are part of the project contract. |

## File a useful report

Use the [bug report template](../../.github/ISSUE_TEMPLATE/bug_report.yml).
Include commit, build mode, target OS, CPU/GPU/driver, exact command, expected
and actual behavior, and the first failure. Prefer synthetic ELF fixtures or
homebrew reproductions. Share relevant text logs with private paths redacted;
do not attach proprietary binaries, firmware or keys.
