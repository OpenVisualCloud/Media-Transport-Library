'use strict';

module.exports = async ({ github, context, core }) => {
  const sha = context.payload.pull_request?.head?.sha || context.sha;
  const checkName = process.env.CHECK_NAME;
  const maxWait = Number.parseInt(process.env.WAIT_TIMEOUT, 10);
  const interval = Number.parseInt(process.env.WAIT_INTERVAL, 10);
  core.info(`Waiting for "${checkName}" on SHA: ${sha}`);

  let activeElapsed = 0;
  while (true) {
    const { data } = await github.rest.checks.listForRef({
      owner: context.repo.owner,
      repo: context.repo.repo,
      ref: sha,
      check_name: checkName,
    });
    const runs = data.check_runs.filter((run) => run.app?.slug === 'github-actions');
    if (runs.some((run) => run.status === 'completed' && run.conclusion === 'success')) return;
    const failed = runs.find((run) => run.status === 'completed' && run.conclusion !== 'success');
    const pending = runs.some((run) => run.status !== 'completed');
    const inProgress = runs.some((run) => run.status === 'in_progress');
    if (failed && !pending) {
      core.setFailed(`"${checkName}" finished with: ${failed.conclusion}`);
      return;
    }
    const state = inProgress ? 'in_progress' : pending ? 'queued' : 'not found';
    core.info(`${checkName}: ${state}; active wait (${activeElapsed}s / ${maxWait}s)`);
    if (inProgress && activeElapsed >= maxWait) break;
    await new Promise((resolve) => setTimeout(resolve, interval * 1000));
    if (inProgress) activeElapsed += interval;
  }
  core.setFailed(`Timed out waiting for "${checkName}"`);
};
