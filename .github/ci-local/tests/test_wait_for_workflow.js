'use strict';

const assert = require('node:assert');
const waitForWorkflow = require('../../actions/wait-for-workflow/wait.js');

const githubRun = (status, conclusion = null) => ({
  app: { slug: 'github-actions' },
  status,
  conclusion,
});

async function run(states, timeout = 600, interval = 30, queueTimeout = 4200, checkNames = 'build') {
  const failures = [];
  const originalSetTimeout = global.setTimeout;
  let poll = 0;
  const polls = new Map();
  process.env.CHECK_NAMES = checkNames;
  delete process.env.CHECK_NAME;
  process.env.WAIT_TIMEOUT = String(timeout);
  process.env.QUEUE_TIMEOUT = String(queueTimeout);
  process.env.WAIT_INTERVAL = String(interval);
  global.setTimeout = (resolve) => resolve();

  try {
    await waitForWorkflow({
      github: {
        rest: {
          checks: {
            listForRef: async ({ check_name: checkName }) => {
              if (Array.isArray(states)) return { data: { check_runs: states[poll++] } };
              const namePoll = polls.get(checkName) || 0;
              polls.set(checkName, namePoll + 1);
              return { data: { check_runs: states[checkName][namePoll] } };
            },
          },
        },
      },
      context: {
        payload: { pull_request: { head: { sha: 'test-sha' } } },
        repo: { owner: 'owner', repo: 'repo' },
      },
      core: { info: () => {}, setFailed: (message) => failures.push(message) },
    });
  } finally {
    global.setTimeout = originalSetTimeout;
    delete process.env.CHECK_NAMES;
  }

  return failures;
}

async function main() {
  const queuedThenSuccess = Array.from({ length: 5 }, () => []);
  queuedThenSuccess.push(...Array.from({ length: 20 }, () => [githubRun('queued')]));
  queuedThenSuccess.push([githubRun('in_progress')]);
  queuedThenSuccess.push([githubRun('completed', 'success')]);
  assert.deepStrictEqual(await run(queuedThenSuccess), []);

  const activeUntilDeadline = Array.from({ length: 20 }, () => [githubRun('in_progress')]);
  activeUntilDeadline.push([githubRun('completed', 'success')]);
  assert.deepStrictEqual(await run(activeUntilDeadline), []);

  const activeUntilTimeout = Array.from({ length: 21 }, () => [githubRun('in_progress')]);
  assert.deepStrictEqual(await run(activeUntilTimeout), ['Timed out waiting for "build"']);

  // A check that never appears is a workflow that was never triggered, not a
  // queue: give up on the same budget rather than holding the runner until the
  // job timeout.
  const neverAppears = Array.from({ length: 21 }, () => []);
  assert.deepStrictEqual(await run(neverAppears), ['Timed out waiting for "build"']);

  // Queued time has a budget of its own, far larger than the active one, so a
  // wait many times longer than the build still succeeds once the hardware frees
  // up. 60 polls of 30s is 30 minutes, well past maxWait and well inside the
  // queue budget.
  const queuedPastBudget = Array.from({ length: 60 }, () => [githubRun('queued')]);
  queuedPastBudget.push([githubRun('completed', 'success')]);
  assert.deepStrictEqual(await run(queuedPastBudget), []);

  // And when that budget does run out, the reason says the fleet never picked
  // the build up. A bare timeout here reads as a failing test on a commit that
  // was never tested, which is what sent this session looking at the code.
  const queuedForever = Array.from({ length: 10 }, () => [githubRun('queued')]);
  const [queueFailure] = await run(queuedForever, 600, 30, 120);
  assert.match(queueFailure, /sat queued for 2 minutes/);
  assert.match(queueFailure, /fleet availability, not a result/);

  const multipleChecks = {
    linux: [[githubRun('completed', 'success')]],
    windows: [[], [githubRun('completed', 'success')]],
  };
  assert.deepStrictEqual(await run(multipleChecks, 600, 30, 4200, 'linux\nwindows'), []);

  console.log('wait-for-workflow state accounting: PASS');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});