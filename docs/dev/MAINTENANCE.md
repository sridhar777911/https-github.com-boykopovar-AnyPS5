# Maintenance

## Continuous integration

| Workflow | Trigger/purpose |
| --- | --- |
| [Build](../../.github/workflows/build.yml) | PR/main/manual; Linux and pinned WinLibs Windows full builds, patched `libs`, repeated CTest; dependency-free relinker build/test on Linux, Windows and macOS |
| [Documentation](../../.github/workflows/docs.yml) | PR/main/manual; local Markdown links and tooling/policy regressions |
| [Conventions](../../.github/workflows/conventions.yml) | PR revisions and metadata against contribution rules |
| [Progress](../../.github/workflows/progress.yml) | Main/manual; generate and publish implementation reports |
| [Progress report](../../.github/workflows/progress-report.yml) | PR; compare implementation counts |
| [Progress comment](../../.github/workflows/progress-comment.yml) | Completed report; publish current-head PR feedback |
| [Release](../../.github/workflows/release.yml) | `v*` tag; build/test/package both OSes, create draft then publish complete assets |

Other workflows synchronize labels, mark merge conflicts and find overlapping
PR implementations. Keep repository-writing permissions scoped to their jobs.
Build/check jobs consume untrusted PR content and should stay read-only.

## Dependencies

Dependencies are Git submodules listed in [.gitmodules](../../.gitmodules).
Update the pinned revision deliberately, explain why it is needed, and run the
affected full builds. Do not switch to ambient system packages to make a local
build pass. FFmpeg's prebuilt-directory option is documented in [Build](BUILD.md).
Do not commit local compiler paths, generated caches or runtime content.

## Documentation policy

Keep maintained guides under `docs/dev/` or `docs/user/`; `docs/README.md` is
the navigation index. Root policy/entry-point files and scoped `AGENTS.md` are
also maintained documentation. `AGENT.md` only points to the root instructions.
Transient reports, notes and logs stay in the PR. Technical gaps belong in
[Technical debt](TechnicalDebt.md).

`tools/check_docs.py` checks working files, including heading fragments. Its
scope is root Markdown, `docs/`, agent guidance under `core/` and `tools/`,
and Markdown templates under `.github/`. It excludes dependencies/build output
and never follows external URLs. Run it before changing navigation or filenames.

The conventions checker independently enforces new-file policy against committed
trees. When changing policy, update its regression tests and
[Contributing](../../CONTRIBUTING.md) together.

## Release packaging

The existing release workflow expects a `v*` tag and validates Linux/Windows
builds before packaging. A relinker-only build is not a full release source.
The packaging script verifies that all expected patched libraries and the
relinker exist and are nonempty.

On the matching supported build host, using an example tag:

```sh
python3 tools/package_release.py --platform linux --build build --output dist --version v0.1.0
```

Windows packaging uses `--platform windows` and also expects MinGW runtime DLLs
in `C:/winlibs/mingw64/bin`, matching CI. It produces `.zip` and `.tar.gz`
library archives and a separately versioned relinker executable. The publication
job also collects user Markdown guides:

```sh
python3 tools/package_release.py --docs docs/user --output dist-docs
```

User documentation asset filenames must be unique. Review checks and artifacts
before publishing a tag; tagging/publishing is a remote action, separate from
local packaging verification.
