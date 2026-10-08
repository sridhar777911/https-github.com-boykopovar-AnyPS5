### What
<!-- Concrete problem and resulting behavior; link related issues when applicable. -->

### Tested
<!-- Commands, OS/toolchain, title or synthetic/homebrew fixture, outcomes, skips and unavailable checks. "Not tested" is a valid answer. -->

### Checklist
<!-- Rules behind each item: CONTRIBUTING.md -->
- [ ] Based on current `main`; no other open PR implements the same functions
- [ ] One topic per PR; follow-ups go in a new PR
- [ ] No comments in code except [technical debt](https://github.com/boykopovar/AnyPS5/blob/main/docs/dev/CONVENTIONS.md)
- [ ] Unimplemented paths throw (`NotImplemented_nid_no_patch`); silent stubs are listed in [TechnicalDebt](https://github.com/boykopovar/AnyPS5/blob/main/docs/dev/TechnicalDebt.md#silent-stubs)
- [ ] Documentation links checked; maintained guides/AGENTS.md follow the documentation policy; transient notes/logs/images are attached to this PR
- [ ] New third-party code is a submodule built from source
- [ ] Generic behaviour, not specific to one title
- [ ] Depends on: <!-- #PR, or none -->
- [ ] AI-assisted: <!-- yes / no -->
