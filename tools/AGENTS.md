# Tools

Read [repository guidance](../AGENTS.md) and
[maintenance guide](../docs/dev/MAINTENANCE.md).

- Repository Python checks use the standard library. Keep checks runnable
  without a full C++ build or third-party submodules.
- Run scripts from the repository root unless their help says otherwise.
  Use `python3 -m unittest discover -s tools/tests -p 'test_*.py'` for regressions.
- `check_conventions.py` compares Git commits; `check_docs.py` inspects current
  files. Preserve this distinction in CLI help and documentation.
- Keep progress counts tied to declared implementation state, not verified
  correctness or game compatibility. Shared-source wrappers count once.
- Release packaging must reject missing/empty assets and preserve asset names
  used by the workflow. Use temporary synthetic trees when testing packaging.
- Put generated progress, oracle kernels and measurements outside tracked
  source. Hardware oracle changes require the Linux AMD/ROCm environment in
  [HW_ORACLE.md](../docs/dev/HW_ORACLE.md).
