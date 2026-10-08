# Repository guidance

## Start here

Read [README.md](README.md), [CONTRIBUTING.md](CONTRIBUTING.md), and the
[documentation index](docs/README.md). Read the nearest nested `AGENTS.md`
before changing a subsystem. Nested guidance supplements this file.
`AGENT.md` is a pointer for tools that use the singular filename.

## Project constraints

- AnyPS5 converts guest x86-64 executables to Linux ELF or Windows PE and
  supplies native system libraries. Conversion on macOS does not imply
  macOS game execution.
- Implement general behavior. Unsupported states throw; do not hide missing
  imports, fabricate success, or add title-specific exceptions.
- Preserve guest ABI, NID exports, module ownership, and binary layout.
  Helpers that must not be exported use `_nid_no_patch` or `_nid_no_patch_cut`.
- The relinker uses only the C++20 standard library. Third-party source belongs
  in pinned submodules under `3rdparty/`; do not edit vendored code incidentally.
- Follow [naming and comment rules](docs/dev/CONVENTIONS.md). Do not add code
  comments as an automated author. Explain changes in documentation and the PR.
- Shader semantics require an exact source or hardware measurements. Record
  unverified behavior in [TechnicalDebt.md](docs/dev/TechnicalDebt.md).
- Keep user changes and unrelated work intact. Inspect `git status` before
  editing. Do not reset branches, commit, push, or publish without authorization.

## Validation

For docs and tooling:

```sh
python3 tools/check_docs.py
python3 -m unittest discover -s tools/tests -p 'test_*.py'
```

For relinker changes (CMake 3.22.1+, Ninja, C++20 compiler, Python 3):

```sh
cmake --preset relinker
cmake --build --preset relinker
ctest --preset relinker
```

For libraries, decoders, or shaders use the [full build](docs/dev/BUILD.md),
build the `libs` target, and run the relevant CTest tests on Linux or Windows.
Run `ctest --test-dir build -N` to discover names; do not assume every test
is registered on every host. GPU skips are not passing hardware validation.

`tools/check_conventions.py` compares committed revisions, not working files.
Run it with the intended base and head after committing; use the documentation
checker and unit tests while editing. Never make a commit solely to run it.

## Completion

Update affected user/developer docs alongside behavior changes. Keep permanent
instructions in maintained docs or `AGENTS.md`; keep transient investigation
notes, logs, and screenshots in the PR. Report changed behavior, checks actually
run, skipped checks and their reason, and any remaining limitation. Do not claim
playability from conversion, compilation, or progress percentages alone.
