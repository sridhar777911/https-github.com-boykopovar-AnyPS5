# GitHub workflows and templates

Read [repository guidance](../AGENTS.md) and
[maintenance guide](../docs/dev/MAINTENANCE.md).

- Default to read-only workflow permissions; grant writes only to the job that
  needs them. Keep untrusted PR code out of privileged jobs.
- Use existing action versions/toolchain pins consistently. Keep Linux and
  Windows full builds, explicit `libs` builds, and CTest coverage intact.
- Documentation checks must run without submodules, GPUs or secrets.
- Issue/PR templates request actionable reproduction and validation evidence.
  Do not ask users to upload proprietary executables, firmware or keys.
- Publishing jobs and scheduled jobs affect remote state. Review their trigger,
  permissions and artifacts; do not dispatch them during local validation.
