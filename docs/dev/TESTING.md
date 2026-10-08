# Testing

## Choose checks by change

| Change | Required evidence |
| --- | --- |
| Docs, agent guidance, templates | Documentation checker, tooling unit tests, diff whitespace check |
| Relinker CLI, parsing, output, CPU lowering | Relinker build and CTest; native execution on affected OS when applicable |
| Library implementation or ABI | Full build, `libs` target, behavioral tests and affected runtime check |
| RDNA semantics or SPIR-V emission | Full build, shader tests, SPIR-V validation, hardware/source evidence |
| Driver synchronization or graphics resources | Full build, relevant AGC tests, Vulkan execution on affected device/OS |
| Decoder | Full build and decoder tests |
| Progress/conventions/docs/packaging tools | Standard-library unit tests and representative synthetic inputs |

## Relinker

```sh
cmake --preset relinker
cmake --build --preset relinker
ctest --preset relinker
ctest --test-dir build-relinker -N
ctest --test-dir build-relinker --output-on-failure -R '^(elf_header|input_magic|string_table_bounds)$'
```

Python 3 is optional for building, but needed for Python regression registration.
Fixtures construct test binaries without proprietary input. Format inspection
runs across hosts; execution checks are conditional on compatible OS/architecture.
Passing on macOS does not verify Linux/Windows process startup.

## Full project

```sh
cmake --preset full
cmake --build --preset full
cmake --build build --target libs --parallel
ctest --preset full
ctest --test-dir build -N
ctest --test-dir build --output-on-failure -R 'agc_shader_disk_cache'
```

Initialize submodules first and use [supported toolchains](BUILD.md).
Tests are registered by their owning CMake files. Discover names rather than
guessing from executable filenames. `libs` refreshes patched runtime libraries;
the default build alone is insufficient for testing updated library behavior.

## Vulkan and shader semantics

The AGC test group registers skip return code `77` and disables shader disk
caching for ordinary runs. A skipped GPU test is missing execution evidence.
On Linux with a suitable Vulkan driver, require execution with:

```sh
ANYPS5_REQUIRE_VULKAN=1 ctest --test-dir build --output-on-failure -R '^agc_driver_'
```

The Linux build workflow uses lavapipe and requires Vulkan. Software Vulkan
execution checks host behavior; RDNA instruction measurements require AMD
hardware and the [oracle](HW_ORACLE.md). Record device, driver, wave mode and
floating-point mode where they affect results.

Configure `-DANYPS5_ENABLE_SPIRV_TOOLS=ON` for validation/optimization of generated
SPIR-V. This is separate from executing a shader and comparing its result.

## Documentation and Python

Repository documentation tooling requires Python 3.9 or newer and uses only the
standard library.

```sh
python3 tools/check_docs.py
python3 -m unittest discover -s tools/tests -p 'test_*.py'
git diff --check
```

The documentation checker reads current maintained Markdown, validates local
files/directories and Markdown heading fragments, and skips fenced code and
external URLs. It makes no network requests. Its unit tests cover invalid
paths/headings and scope boundaries. Conventions policy is tested separately.

`check_conventions.py --base <base> --head <head>` checks committed diffs,
including naming of commits, new-file placement and relative links. The
[Conventions workflow](../../.github/workflows/conventions.yml) supplies the PR
base/head revisions.

## Report results

State the commands, host/toolchain, test totals, skipped tests and relevant
environment. For runtime claims include title/version, target OS, CPU/GPU and
observed behavior. A conversion success, passing structural test or progress
percentage does not establish playability.
