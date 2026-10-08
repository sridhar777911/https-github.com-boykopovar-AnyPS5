# AGC driver

Read [library guidance](../../AGENTS.md) and
[shader guidance](../../../shader/recompiler/AGENTS.md) for recompiler changes.

- Follow the command flow from `Submit/` into `Execution/` and `Graphics/`.
  Preserve PM4 state, synchronization, resource lifetimes and guest memory
  visibility across draws, dispatches and presentation.
- Check `CMakeLists.txt` for test registration. Execution fixtures live in
  `tests/execution/`; the wider `tests/` directory covers driver, shader and
  cache behavior.
- CTest uses return code 77 for GPU skips in the registered AGC test group.
  Use `ANYPS5_REQUIRE_VULKAN=1` for checks that must execute on Vulkan.
- Rebuild `libs` before runtime checks. Report device and driver, relevant
  environment variables, and whether tests used hardware or a software driver.
- Avoid title-specific state overrides, ignored packets, silent synchronization
  failures, and cache keys that omit state affecting generated code.
