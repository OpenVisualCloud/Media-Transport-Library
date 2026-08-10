'use strict';

module.exports = async ({ github, context, core }) => {
  const sha = context.payload.pull_request?.head?.sha || context.sha;
  const checkName = process.env.CHECK_NAME;
  const maxWait = Number.parseInt(process.env.WAIT_TIMEOUT, 10);
  const interval = Number.parseInt(process.env.WAIT_INTERVAL, 10);
  core.info(`Waiting for "${checkName}" on SHA: ${sha}`);

  for (let elapsed = 0; elapsed < maxWait; elapsed += interval) {
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
    if (failed && !pending) {
      core.setFailed(`"${checkName}" finished with: ${failed.conclusion}`);
      return;
    }
    core.info(`${checkName}: ${pending ? 'in_progress' : 'not found'}; waiting (${elapsed}s / ${maxWait}s)`);
    await new Promise((resolve) => setTimeout(resolve, interval * 1000));
  }
  core.setFailed(`Timed out waiting for "${checkName}"`);
};