# Development workflow

## Set up

Use the Git checkout root. Read [Contributing](../../CONTRIBUTING.md),
[Architecture](ARCHITECTURE.md), and the [Repository map](REPOSITORY_MAP.md).
Check `git status --short` before editing and look for overlapping open PRs.
Start a topic branch from the intended base; preserve unrelated local work.

For conversion work, use the dependency-free build:

```sh
cmake --preset relinker
cmake --build --preset relinker
ctest --preset relinker
```

`relinker-debug` uses `Debug` in `build-relinker-debug/`. The `full` preset uses
GCC, initializes no dependencies itself, and requires the supported Linux or
Windows environment from [Build](BUILD.md):

```sh
git submodule update --init --recursive
cmake --preset full
cmake --build --preset full
cmake --build build --target libs --parallel
ctest --preset full
```

For custom compilers/options, use the explicit commands in the build guide or
an untracked `CMakeUserPresets.json`. Keep machine-specific paths out of shared
presets. CMake 3.22.1+ is the documented baseline for these presets.

## Make a change

1. Locate the owning subsystem and read its scoped `AGENTS.md`.
2. Reproduce the failure with a synthetic fixture or small behavioral test.
3. Implement the general behavior while preserving ABI and failure handling.
4. Run the relevant checks from [Testing](TESTING.md).
5. Update affected usage/build/architecture docs. Record unknowns and permitted
   silent stubs in [Technical debt](TechnicalDebt.md).

For PRs that change instruction semantics, provide source references or
measurements, including GPU and test inputs. Use the [hardware oracle](HW_ORACLE.md)
when available; explicitly state when hardware validation remains outstanding.

## Review locally

```sh
python3 tools/check_docs.py
python3 -m unittest discover -s tools/tests -p 'test_*.py'
git diff --check
git diff --stat
```

After the intended changes are committed, compare them with the PR base:

```sh
python3 tools/check_conventions.py --base origin/main --head HEAD
```

This checker reads committed revisions. It does not validate uncommitted or
untracked edits. Use the correct base for dependent PRs.

## Submit

Use a Conventional Commit title, such as `docs: add contributor onboarding`.
Fill in the [PR template](../../.github/pull_request_template.md) with the
problem, resulting behavior, actual validation, dependencies and AI assistance.
Attach transient logs and investigation notes to the PR. State unavailable
platform/GPU validation clearly; do not infer game compatibility from unit tests.
