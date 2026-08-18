'use strict';

const assert = require('node:assert');
const waitForWorkflow = require('../../actions/wait-for-workflow/wait.js');

const githubRun = (status, conclusion = null) => ({
  app: { slug: 'github-actions' },
  status,
  conclusion,
});

async function run(states, timeout = 600, interval = 30) {
  const failures = [];
  const originalSetTimeout = global.setTimeout;
  let poll = 0;
  process.env.CHECK_NAME = 'build';
  process.env.WAIT_TIMEOUT = String(timeout);
  process.env.WAIT_INTERVAL = String(interval);
  global.setTimeout = (resolve) => resolve();

  try {
    await waitForWorkflow({
      github: {
        rest: {
          checks: {
            listForRef: async () => ({ data: { check_runs: states[poll++] } }),
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

  console.log('wait-for-workflow state accounting: PASS');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});