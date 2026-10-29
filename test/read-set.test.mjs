// Holds the pre-commit hook's archive pathspec equal to the linter's exported read-scope
// (ADR-0018, ADR-0099). The hook materialises the staged tree by archiving only the paths the
// checks read, a measured cost decision; if the linter grows to read a path the hook does not
// archive, the check runs against a tree missing that file. That drift shipped once (R14/R15 read
// four paths the hook never archived, dead in the hook for a release), which is why this test
// exists. The first test on the repo's node --test lane (ADR-0099).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { READ_SCOPE } from '../scripts/lint-docs.mjs';

// Repo root is the parent of this file's directory (test/). Resolving from the file rather than
// process.cwd() lets the test run from anywhere: CI, the pre-push hook, or a manual node --test.
const repoRoot = dirname(dirname(fileURLToPath(import.meta.url)));

/**
 * The hook's archive pathspec is the single `for candidate in … ; do` line in the committed hook
 * SOURCE under .claude/hooks (what /init-method symlinks into .git/hooks). We read the source, not
 * the .git/hooks symlink, so the test works in a fresh clone and in a CI checkout where .git/hooks
 * is empty.
 */
function hookArchivePaths() {
  const hook = readFileSync(join(repoRoot, '.claude', 'hooks', 'pre-commit'), 'utf8');
  const match = hook.match(/for candidate in (.+?); do/);
  assert.ok(
    match,
    'could not find the `for candidate in … ; do` archive list in .claude/hooks/pre-commit — '
      + 'the hook was restructured; update this test to match (ADR-0018).',
  );
  return match[1].trim().split(/\s+/);
}

test('the pre-commit archive pathspec equals the linter read-scope (ADR-0018, ADR-0099)', () => {
  const hookPaths = new Set(hookArchivePaths());
  const scopePaths = new Set(READ_SCOPE);

  const onlyInHook = [...hookPaths].filter((p) => !scopePaths.has(p)).sort();
  const onlyInScope = [...scopePaths].filter((p) => !hookPaths.has(p)).sort();

  // Report the symmetric difference, not just "not equal": a drift message that names the offending
  // path is what makes the next R14/R15 a one-line fix instead of a hunt.
  assert.deepEqual(
    { onlyInHook, onlyInScope },
    { onlyInHook: [], onlyInScope: [] },
    'the pre-commit archive list and the linter READ_SCOPE have drifted — a path read by one must '
      + 'be archived by the other, or the hook checks a tree missing it (ADR-0018 R14/R15).',
  );
});
