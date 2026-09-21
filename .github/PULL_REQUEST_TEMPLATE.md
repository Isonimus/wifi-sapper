## Summary

<!-- What does this change do, and why? Explain the reasoning, not just the diff. -->

Closes #

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor / cleanup
- [ ] Documentation
- [ ] Build / CI / tooling

## Testing

<!-- Which checks did you run? Paste relevant output. -->

- [ ] `npm run lint` passes (documentation linter)
- [ ] `pio test -e native` passes (host unit suite)
- [ ] `pio run -e cardputer` builds
- [ ] Tested on hardware — which verify script / device?

## Checklist

- [ ] Follows the standards in [docs/quality-bar.md](../docs/quality-bar.md) and [CLAUDE.md](../CLAUDE.md)
- [ ] Added or updated tests for any changed logic (fail-before / pass-after)
- [ ] Behaviour a unit test cannot assert ships a wired `scripts/<slice>-verify.mjs`
- [ ] Conventional-commit messages, one logical change per commit, why-not-what in the body
- [ ] This change is for authorized, lawful use (see [DISCLAIMER](../DISCLAIMER.md))
