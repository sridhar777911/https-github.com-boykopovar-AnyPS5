# Documentation

Read [repository guidance](../AGENTS.md). Keep [the index](README.md) current.

- `user/` covers conversion and runtime setup; `dev/` covers implementation,
  testing and maintenance. Keep one authoritative page per topic and link to it.
- Verify commands, options, test names and paths against source/CMake/workflows.
  Distinguish full runtime support from relinker build support on macOS.
- Compatibility results need a recorded OS/title/device/test outcome. Do not
  imply that progress percentages prove correctness or playability.
- Keep durable guides concise. Investigation notes and logs belong in the PR;
  known gaps, unknown NIDs and signatures belong in `dev/TechnicalDebt.md`.
- Use relative links inside the repository and fenced commands with an
  appropriate language. Do not add binary images.
- Run `python3 tools/check_docs.py` from the repository root. It checks local
  Markdown paths and headings, not external website availability.
