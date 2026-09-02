'use strict';

// Tests for the build/linter gate in wait.js. Built-in `node:test` only, so
// there is nothing to install and no package.json to track. Run with
//
//   node --test .github/actions/wait-for-workflow/wait.test.js
//
// The explicit file, not the directory: the test runner's own discovery skips
// dot-directories, so every path under .github is invisible to it and the
// directory form ends up trying to execute the directory itself. linter.yml's
// `gate-tests` job runs the command above verbatim.
//
// Case 1 pins the defect these tests exist for. doc/coding_standard.md section
// 4.1 has the account of it.

const test = require('node:test');
const assert = require('node:assert/strict');

const wait = require('./wait.js');

const OWNER = 'OpenVisualCloud';
const REPO = 'Media-Transport-Library';
const SHA = '0aff97f0b4cbf68be4a8db76cc00502d8b1dad23';

// Both real, both on that one sha: the Pages deployment's suite and the Build
// workflow's. The whole fix is telling them apart.
const PAGES_SUITE = 90817961263;
const BUILD_SUITE = 90817969271;
// Synthetic -- linter.yml was not part of the incident, only of the contract.
const LINTER_SUITE = 90818000001;

const LINTER_NAMES = [
  'checkpatch (ubuntu-latest)',
  'checkpatch (macos-latest)',
  'checkpatch (windows-latest)',
  'Lint checks not yet in checkpatch',
];

// suite: null models the schema honestly -- check_suite is an optional property
// of a check run, so a response may omit it entirely.
function checkRun(name, { status = 'completed', conclusion = 'success', suite = BUILD_SUITE } = {}) {
  const run = { name, status, conclusion, app: { slug: 'github-actions' } };
  if (suite !== null) run.check_suite = { id: suite };
  return run;
}

// Likewise check_suite_id on a workflow run.
function workflowRun(suite) {
  return suite === null ? {} : { check_suite_id: suite };
}

// `checks` and `runs` are functions of the poll count, so a case can model a
// check run or a workflow run that only comes into existence on a later pass.
// state.polls counts checks.listForRef calls already made: with a single check
// name it is the zero-based pass number, and with the four linter names it also
// advances within a pass.
//
// interval is seconds and must never be 0: the budgets advance by `interval`,
// so `waited.missing += 0` never reaches maxWait and the loop never ends.
async function runGate({
  names,
  workflow = 'build.yml',
  timeout = '2',
  queueTimeout = '600',
  interval = '1',
  checks,
  runs,
}) {
  const state = { polls: 0 };
  const info = [];
  const failed = [];
  const polled = [];
  const lookups = [];
  const github = {
    rest: {
      checks: {
        listForRef: async ({ check_name: checkName }) => {
          polled.push(checkName);
          const matching = checks(state).filter((run) => run.name === checkName);
          state.polls += 1;
          return { data: { check_runs: matching } };
        },
      },
      actions: {
        listWorkflowRuns: async (params) => {
          lookups.push(params);
          return { data: { workflow_runs: runs(state, params.workflow_id) } };
        },
      },
    },
  };

  process.env.CHECK_NAMES = names.join('\n');
  process.env.WORKFLOW_FILE = workflow;
  process.env.WAIT_TIMEOUT = timeout;
  process.env.QUEUE_TIMEOUT = queueTimeout;
  process.env.WAIT_INTERVAL = interval;

  await wait({
    github,
    context: { repo: { owner: OWNER, repo: REPO }, sha: SHA, payload: {} },
    core: { info: (message) => info.push(message), setFailed: (message) => failed.push(message) },
  });

  return { info, failed, polled, lookups };
}

test('a Pages check run named build does not satisfy the Build gate', async () => {
  const { failed, lookups } = await runGate({
    names: ['build'],
    checks: () => [checkRun('build', { suite: PAGES_SUITE })],
    runs: () => [workflowRun(BUILD_SUITE)],
  });
  assert.deepEqual(failed, ['Timed out waiting for "build"']);
  assert.deepEqual(lookups[0], {
    owner: OWNER,
    repo: REPO,
    workflow_id: 'build.yml',
    head_sha: SHA,
    per_page: 100,
  });
});

test('a build check run inside the Build suite satisfies the gate', async () => {
  const { failed } = await runGate({
    names: ['build'],
    checks: () => [checkRun('build', { suite: PAGES_SUITE }), checkRun('build', { suite: BUILD_SUITE })],
    runs: () => [workflowRun(BUILD_SUITE)],
  });
  assert.deepEqual(failed, []);
});

test('a cancelled Build check falls back to not found instead of failing', async () => {
  const { info, failed } = await runGate({
    names: ['build'],
    checks: () => [checkRun('build', { conclusion: 'cancelled' })],
    runs: () => [workflowRun(BUILD_SUITE)],
  });
  assert.deepEqual(failed, ['Timed out waiting for "build"']);
  assert.match(info.join('\n'), /^build: not found;/m);
});

test('a queued Build check ends with the fleet-availability message', async () => {
  const { failed } = await runGate({
    names: ['build'],
    timeout: '600',
    queueTimeout: '1',
    checks: () => [checkRun('build', { status: 'queued', conclusion: null })],
    runs: () => [workflowRun(BUILD_SUITE)],
  });
  assert.equal(failed.length, 1);
  assert.match(failed[0], /sat queued for 0 minutes/);
  assert.match(failed[0], /bare-metal fleet availability/);
  assert.doesNotMatch(failed[0], /Timed out/);
});

test('all four linter check names in the Linter suite satisfy the gate', async () => {
  const { failed, polled } = await runGate({
    names: LINTER_NAMES,
    workflow: 'linter.yml',
    checks: () => LINTER_NAMES.map((name) => checkRun(name, { suite: LINTER_SUITE })),
    runs: (state, workflow) => (workflow === 'linter.yml' ? [workflowRun(LINTER_SUITE)] : []),
  });
  assert.deepEqual(failed, []);
  assert.deepEqual([...new Set(polled)].sort(), [...LINTER_NAMES].sort());
});

test('an omitted check_suite does not match an omitted check_suite_id', async () => {
  // Both properties are optional in GitHub's schema. Build the suite set without
  // dropping the absent ones and `undefined` lands in it, at which point it
  // matches every check run that omits check_suite -- the Pages one included --
  // and the gate is fail-open again with nothing in the log to say so.
  const { failed } = await runGate({
    names: ['build'],
    checks: () => [checkRun('build', { suite: null })],
    runs: () => [workflowRun(null)],
  });
  assert.deepEqual(failed, ['Timed out waiting for "build"']);
});

test('an in_progress Build check spends the active budget and times out', async () => {
  const { info, failed } = await runGate({
    names: ['build'],
    checks: () => [checkRun('build', { status: 'in_progress', conclusion: null })],
    runs: () => [workflowRun(BUILD_SUITE)],
  });
  assert.deepEqual(failed, ['Timed out waiting for "build"']);
  assert.match(info.join('\n'), /^build: in_progress; waited 0s in_progress/m);
  assert.match(info.join('\n'), /^build: in_progress; waited 1s in_progress/m);
});

test('a workflow filename is required, and nothing is polled without one', async () => {
  // `required: true` on a composite action's input is documentation, not
  // enforcement: an omitted input arrives as the empty string, and an unscoped
  // wait is exactly the defect above.
  const { failed, polled, lookups } = await runGate({
    names: ['build'],
    workflow: '',
    checks: () => [checkRun('build')],
    runs: () => [workflowRun(BUILD_SUITE)],
  });
  assert.deepEqual(failed, ['workflow is empty']);
  assert.deepEqual(polled, []);
  assert.deepEqual(lookups, []);
});

test('the suite lookup stops once a suite is in scope, and the set survives', async () => {
  // The scope lookup is a second API call per pass. A queue wait can reach 140
  // passes against GITHUB_TOKEN's 1000 requests an hour, so it drops to every
  // fourth pass as soon as anything is in scope. This pins that cadence, and with
  // it the reason the set accumulates rather than being replaced: the passes in
  // between re-derive nothing, so whatever was seen has to keep counting. The
  // mock returns runs on the first pass only to make a skipped refresh visible --
  // not a claim that the real endpoint ever drops a run from a single-sha query.
  const { failed, lookups } = await runGate({
    names: ['build'],
    timeout: '10',
    checks: (state) => (state.polls >= 4 ? [checkRun('build')] : []),
    runs: (state) => (state.polls === 0 ? [workflowRun(BUILD_SUITE)] : []),
  });
  assert.deepEqual(failed, []);
  assert.equal(lookups.length, 2);
});

test('a Build run created after the first poll is picked up, not cached away', async () => {
  // The gate legitimately starts before the Build run exists: 13:49:33 against a
  // DPDK cache saved at 13:51:36. Run and check both appear only on the second
  // pass, so a suite lookup hoisted above the poll loop caches the empty set and
  // turns the old false pass into a permanent false failure.
  const { info, failed } = await runGate({
    names: ['build'],
    checks: (state) => (state.polls === 0 ? [] : [checkRun('build')]),
    runs: (state) => (state.polls === 0 ? [] : [workflowRun(BUILD_SUITE)]),
  });
  assert.deepEqual(failed, []);
  assert.match(info.join('\n'), /^build: not found;/m);
});
