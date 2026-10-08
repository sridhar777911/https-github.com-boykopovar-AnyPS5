# Relinker

Read [repository guidance](../../AGENTS.md), [usage](../../docs/user/USAGE.md),
and [architecture](../../docs/dev/ARCHITECTURE.md).

- Keep the conversion tool dependency-free: C++20 standard library only.
- `cli/` parses options and handles autorun; `domain/` defines shared contracts;
  `io/` reads/writes bounded byte sequences; `codegen/` decodes and lowers CPU
  instructions; `relinker/` parses imports and builds conversion data;
  `elfpatcher/` writes Linux and Windows output.
- Treat input binaries as untrusted. Check lengths, offsets, alignment,
  overflow, virtual-address/file-offset conversions, and string termination
  before access. Throw descriptive errors for unsupported or malformed input.
- Preserve bundled guest module paths and distinguish them from host system
  libraries. Both output formats must remain inspectable on any build host.
- `.exe` does not select Windows output: `--windows` does. Keep argument
  validation and exit codes consistent with the usage guide.
- Use synthetic ELF fixtures in `relinker/tests/` for format regressions and
  C++ tests under `cli/tests/`, `io/tests/`, `codegen/tests/`, or
  `elfpatcher/tests/` for their behavior. Register new tests in `CMakeLists.txt`.
- Run the relinker preset and CTest. Host-specific execution needs its native
  OS/architecture; parsing a PE on macOS does not verify Windows startup.
