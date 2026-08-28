'use strict';

module.exports = async ({ github, context, core }) => {
  const sha = context.payload.pull_request?.head?.sha || context.sha;
  const checkNames = (process.env.CHECK_NAMES || process.env.CHECK_NAME || '')
    .split('\n')
    .map((name) => name.trim())
    .filter(Boolean);
  const maxWait = Number.parseInt(process.env.WAIT_TIMEOUT, 10);
  const queueWait = Number.parseInt(process.env.QUEUE_TIMEOUT || '4200', 10);
  const interval = Number.parseInt(process.env.WAIT_INTERVAL, 10);
  if (!checkNames.length) {
    core.setFailed('check_name is empty');
    return;
  }
  core.info(`Waiting for ${checkNames.map((name) => `"${name}"`).join(', ')} on SHA: ${sha}`);

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
  const elapsed = new Map(checkNames.map((name) => [name, { active: 0, missing: 0, queued: 0 }]));
  const passed = new Set();
  while (true) {
    for (const checkName of checkNames) {
      if (passed.has(checkName)) continue;
      const { data } = await github.rest.checks.listForRef({
        owner: context.repo.owner,
        repo: context.repo.repo,
        ref: sha,
        check_name: checkName,
      });
      const runs = data.check_runs.filter((run) => run.app?.slug === 'github-actions');
      if (runs.some((run) => run.status === 'completed' && run.conclusion === 'success')) {
        passed.add(checkName);
        continue;
      }
      // A cancelled or skipped check is not a result. Two events on one commit --
      // a push and the pull_request synchronize it causes -- give the commit two
      // run sets, and the concurrency group cancels the older one. That leaves a
      // completed/cancelled "build" check on the same sha while the live build has
      // not created its check yet, so reading the cancel as a failure ends this
      // gate in seconds on a build that is about to start. Dropping those runs
      // lets the state fall back to "not found", and the absent budget then waits
      // for the live check.
      const live = runs.filter((run) => run.conclusion !== 'cancelled' && run.conclusion !== 'skipped');
      const failed = live.find((run) => run.status === 'completed' && run.conclusion !== 'success');
      const pending = live.some((run) => run.status !== 'completed');
      const inProgress = live.some((run) => run.status === 'in_progress');
      if (failed && !pending) {
        core.setFailed(`"${checkName}" finished with: ${failed.conclusion}`);
        return;
      }
      const state = inProgress ? 'in_progress' : pending ? 'queued' : 'not found';
      const waited = elapsed.get(checkName);
      core.info(
        `${checkName}: ${state}; waited ${waited.active}s in_progress, ` +
          `${waited.queued}s queued and ${waited.missing}s absent ` +
          `(${maxWait}s allowed active or absent, ${queueWait}s queued)`
      );
      if ((inProgress && waited.active >= maxWait) || (state === 'not found' && waited.missing >= maxWait)) {
        core.setFailed(`Timed out waiting for "${checkName}"`);
        return;
      }
      if (state === 'queued' && waited.queued >= queueWait) {
        core.setFailed(
          `"${checkName}" sat queued for ${Math.round(waited.queued / 60)} minutes ` +
            'without a runner picking it up, so this commit was never tested. That is ' +
            'bare-metal fleet availability, not a result: re-run this job once a ' +
            'runner for the build host is online.'
        );
        return;
      }
      waited[inProgress ? 'active' : state === 'queued' ? 'queued' : 'missing'] += interval;
    }
    if (passed.size === checkNames.length) return;
    await new Promise((resolve) => setTimeout(resolve, interval * 1000));
  }
};
