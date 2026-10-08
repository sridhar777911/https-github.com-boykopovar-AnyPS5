# Shader recompiler

Read [repository guidance](../../../AGENTS.md),
[architecture](../../../docs/dev/ARCHITECTURE.md), and the
[hardware oracle guide](../../../docs/dev/HW_ORACLE.md).

- Keep the stage boundaries: `RdnaDecoder` → `ControlFlow` → `Translation` →
  `Optimization` → `SpirvBackend`. `IntermediateRepresentation` defines the
  values and operations shared by translation and optimization.
- Instruction changes need exact ISA/compiler-source evidence or hardware
  measurements. Include GPU, wave size, floating-point mode, input rows and
  expected outputs. Unsupported ambiguity throws.
- Preserve EXEC masks, lane/subgroup behavior, denormal/NaN/rounding semantics,
  register widths, and descriptor interpretation explicitly.
- Tests for emitted shaders and GPU execution are primarily registered by
  [libSceAgcDriver](../../libs/prx/libSceAgcDriver/CMakeLists.txt). Use the full
  build; the relinker preset does not exercise this subsystem.
- Validate generated SPIR-V with `ANYPS5_ENABLE_SPIRV_TOOLS=ON` when changing
  emission. Hardware execution and SPIR-V validation prove different things.
- Cache changes require reviewing `CacheKey.hpp`, `ShaderDiskCache.cpp`,
  `ShaderCacheVersion.cmake`, and cache tests. Keep serialization and version
  inputs aligned with changed types and semantics.
- Record unverified behavior in
  [TechnicalDebt.md](../../../docs/dev/TechnicalDebt.md); do not invent measured
  results or count skipped GPU tests as passing execution.
