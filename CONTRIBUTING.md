# Contributing

Start with the [development workflow](docs/dev/DEVELOPMENT.md) and
[repository map](docs/dev/REPOSITORY_MAP.md). Automated contributors also read
[AGENTS.md](AGENTS.md) and the guidance in the directories they change.

## Code

- Follow the [coding conventions](docs/dev/CONVENTIONS.md): naming, no comments except [technical debt](docs/dev/TechnicalDebt.md), Conventional Commits.
- Every function either does exactly what it is supposed to or throws. Unimplemented exports call `NotImplemented_nid_no_patch(__func__)` (see [libSceAudioIn](core/libs/prx/libSceAudioIn/Export.cpp)).
- Pull requests that add or change shader instruction semantics say where they come from: measured on hardware (which GPU and what was checked, e.g. with the [hardware oracle](docs/dev/HW_ORACLE.md)) or the exact source (ISA section, LLVM, ACO or Mesa file). Reviewers check the semantics on hardware. Cases the source doesn't settle throw, and behaviour not verified on hardware is recorded in [technical debt](docs/dev/TechnicalDebt.md).
- A silent stub is allowed only when it unblocks a title and only affects the UI; add it to [silent stubs](docs/dev/TechnicalDebt.md#silent-stubs).
- Missing imports fail at startup with a clear error; don't replace them with fallbacks that keep running.
- Implement the general behaviour of a function, not what one title happens to need.
- Don't add replacements for modules that titles ship themselves in `sce_module/`, `sce_modules/` or `prx/`: engine or middleware modules (Cohtml, FMOD, GOG Galaxy) and SDK libraries that only wrap other system libraries (NpCppWebApi over NpWebApi2). The relinker converts and loads the title's own module, and a host library with the same name is left out of `DT_NEEDED`. Only system libraries, which reach the kernel or the hardware, are reimplemented in [core/libs/prx](core/libs/prx).
- Avoid non-standard extensions (`__attribute__`, etc.) where standard C++ is enough. Helper symbols that must not become NIDs use the `_nid_no_patch` or `_nid_no_patch_cut` suffix.
- The relinker uses only the C++20 standard library.
- Third-party code is added as a submodule under `3rdparty/` and built from source, not found on the system.
- Don't add tests that only check that a symbol is exported: a missing export already fails at startup.

## Build and test

Toolchains are listed in the [build instructions](docs/dev/BUILD.md).

For relinker changes, use the [relinker-only build](docs/dev/BUILD.md#relinker-only) to build and test without third-party submodules, system libraries or a GPU. It also works on macOS. Changes to system libraries or shaders still need the full build and relevant runtime tests.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build
cmake --build build --target libs
ctest --test-dir build --output-on-failure
```

Python 3 is optional for building; without it Python regression tests are not
registered. See [Testing](docs/dev/TESTING.md) for scope and GPU requirements.
For current documentation and tooling changes, run:

```sh
python3 tools/check_docs.py
python3 -m unittest discover -s tools/tests -p 'test_*.py'
```

The Conventions check runs on every pull request and enforces the rules on this
page. It accepts code comments only when the pull request also changes
[TechnicalDebt](docs/dev/TechnicalDebt.md), and only UTF-8 text files. It compares
committed revisions and does not inspect working-tree edits. Run it locally
against the intended PR base after committing and before pushing:

```
python3 tools/check_conventions.py --base origin/main
```

## Branches and pull requests

The [pull request template](.github/pull_request_template.md) is the checklist for these rules.

- One branch per topic, based on current `main`. Follow-up work goes in a new pull request, not into an open one.
- Before starting, check that no open pull request already implements the same functions.
- Keep the branch up to date with `main` and resolve conflicts yourself; rebasing and force-pushing is fine.
- If a pull request needs another one first, say so in the description (`Depends on #N`).
- Run the tests before opening the pull request and describe what was tested (OS, title or homebrew).
- Investigation notes, reports, screenshots and logs go in the pull request, not in the repository. Images for documentation go in the [gist](https://gist.github.com/boykopovar/0e53f2e1426f29ecd41e3b51540b8a90) comments.
- Say whether the change was written with AI assistance. The author of the pull request is responsible for every line of it.

## Documentation

Keep documentation concise, factual and grounded in source. Add durable user or
developer guides under `docs/user/` or `docs/dev/` and link them from
[docs/README.md](docs/README.md). Root `README.md`, `CONTRIBUTING.md`, `AGENTS.md`
and the `AGENT.md` pointer are maintained entry points. Scoped `AGENTS.md` files
are allowed under `core/`, `tools/`, `docs/` and `.github/`; keep their rules
specific to the owning subsystem. Do not add alternate agent instruction files
or transient investigation reports.

Record known gaps, unknown NIDs and signatures in
[technical debt](docs/dev/TechnicalDebt.md). Update affected docs with behavior
changes. Run `python3 tools/check_docs.py` to validate current local links and
heading fragments. See [Maintenance](docs/dev/MAINTENANCE.md) for CI and release
responsibilities.
