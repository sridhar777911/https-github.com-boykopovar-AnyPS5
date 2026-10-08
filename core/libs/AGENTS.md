# Native libraries

Read [repository guidance](../../AGENTS.md) and
[build instructions](../../docs/dev/BUILD.md).

- `prx/<library>/` owns a native system library. Preserve `APS5_VABI` signatures,
  guest structure layout, calling convention, export identity, and error codes.
- `nid/` patches native export names into NIDs. Check its exclusion rules when
  adding helpers; accidental helper exports alter the library interface.
- Unknown implementations call `NotImplemented_nid_no_patch(__func__)`.
  UI-only silent stubs need an explicit entry in
  [technical debt](../../docs/dev/TechnicalDebt.md#silent-stubs).
- Do not reimplement title-bundled middleware. Use the converted guest module.
- Keep Linux/Windows platform-specific implementations behind the existing
  boundaries. Test platform changes on the affected OS.
- `libs` is a separate build target. Rebuild it and copy the patched files from
  `build/core/libs/libs/` to the runtime before checking a title. Files under
  `unpatched/` are build intermediates.
- Test behavior, resource ownership and ABI edge cases; export-existence-only
  tests provide no additional coverage over loader startup.
