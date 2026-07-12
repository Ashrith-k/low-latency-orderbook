# CLAUDE.md — project conventions for Claude Code

Read DESIGN.md and PLAN.md before any task. Follow them; propose changes instead of
silently deviating.

## Workflow rules
- Implement ONE task from PLAN.md at a time. Stop after each task and wait for review.
- Before writing code, state in 3–5 bullets: the approach, key trade-offs, and what
  the tests will assert.
- Every feature commit ships with its tests in the same commit. No untested code.
- Keep commits small; use the commit messages suggested in PLAN.md.
- Never dump multiple modules in one response.
- If a design decision is ambiguous, ask — do not guess.

## Code rules
- C++20. No exceptions on the hot path; no heap allocation after engine startup.
- No raw `new`/`delete`; pool indices instead of pointers for book structures.
- Fixed-point integer prices only. `static_assert` all POD sizes/alignments.
- Warnings are errors. Code must be clean under ASan, UBSan, TSan and clang-tidy.
- Document memory-ordering choices at every atomic with a one-line rationale.
- Google style via .clang-format, 100 columns.

## Commands
- Configure: `cmake --preset debug` (also: asan, tsan, release)
- Build: `cmake --build --preset debug`
- Test: `ctest --preset debug --output-on-failure`
- Format: `./scripts/format.sh`

## Definition of done (per task)
Builds on gcc+clang, tests pass, sanitizers clean, formatted, PLAN.md task checked off.
