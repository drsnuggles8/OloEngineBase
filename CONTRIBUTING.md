# Contributing to OloEngineBase

Thanks for your interest in contributing! 🎉

## Pre-commit hooks (style & sanity checks) ✅
We use `pre-commit` to enforce formatting and basic repository checks (e.g., trailing whitespace, EOF fixes, clang-format).

Please run hooks locally before opening a PR:

1. Install pre-commit: `pip install pre-commit`
2. Install the git hooks in your repo: `pre-commit install`
3. Run all hooks across the repo to verify and auto-fix issues: `pre-commit run --all-files`

If CI fails because hooks made auto-fixes, run the command above, review the changes, commit them, and push again.

### Fix warnings about deprecated hook stages
If you see warnings about deprecated stages in `pre-commit` hooks (e.g. `commit`, `push`), update the affected hook repo:

- `pre-commit autoupdate --repo https://github.com/pre-commit/pre-commit-hooks`

(Repeat for other repos listed in `.pre-commit-config.yaml` if necessary.)

---
If you want a different formatting convention (spacing around pointers, template keyword), open an issue or PR to discuss changes to `.clang-format`.
