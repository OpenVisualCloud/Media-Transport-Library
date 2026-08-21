'use strict';

module.exports = async ({ github, context, core }) => {
  const sha = context.payload.pull_request?.head?.sha || context.sha;
  const checkName = process.env.CHECK_NAME;
  const maxWait = Number.parseInt(process.env.WAIT_TIMEOUT, 10);
  const queueWait = Number.parseInt(process.env.QUEUE_TIMEOUT || '4200', 10);
  const interval = Number.parseInt(process.env.WAIT_INTERVAL, 10);
  core.info(`Waiting for "${checkName}" on SHA: ${sha}`);

  // Three budgets, because the three ways of waiting are not the same thing.
  // Time spent while the check is in_progress is the build itself and is bounded
  // by maxWait. Time spent while it does not exist yet is bounded by the same
  // number, so a wait for a check that will never be created fails in minutes
  // rather than holding a runner until the job timeout.
  //
  // Queued time gets its own, much larger budget: on this fleet the queue in
  // front of a single shared host is routinely longer than the build, and giving
  // up on a commit because the hardware was busy is the failure mode this action
  // exists to avoid. It is bounded all the same, and deliberately just inside
  // the enclosing job's timeout-minutes, because the two endings do not read the
  // same. Letting the runner kill the job produces "exceeded the maximum
  // execution time", which says nothing about hardware; ending it here says the
  // fleet never picked the build up, which is the one thing the reader needs and
  // cannot see from a red check.
  let activeElapsed = 0;
  let missingElapsed = 0;
  let queuedElapsed = 0;
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
    core.info(
      `${checkName}: ${state}; waited ${activeElapsed}s in_progress, ` +
        `${queuedElapsed}s queued and ${missingElapsed}s absent ` +
        `(${maxWait}s allowed active or absent, ${queueWait}s queued)`
    );
    if (inProgress && activeElapsed >= maxWait) break;
    if (state === 'not found' && missingElapsed >= maxWait) break;
    if (state === 'queued' && queuedElapsed >= queueWait) {
      core.setFailed(
        `"${checkName}" sat queued for ${Math.round(queuedElapsed / 60)} minutes ` +
          'without a runner picking it up, so this commit was never tested. That is ' +
          'bare-metal fleet availability, not a result: re-run this job once a ' +
          'runner for the build host is online.'
      );
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, interval * 1000));
    if (inProgress) activeElapsed += interval;
    if (state === 'not found') missingElapsed += interval;
    if (state === 'queued') queuedElapsed += interval;
  }
  core.setFailed(`Timed out waiting for "${checkName}"`);
};
