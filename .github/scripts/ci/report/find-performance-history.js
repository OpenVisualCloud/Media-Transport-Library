'use strict';

// The last nightly performance run of main before this run that kept the session history
// of this runner's host, which performance.yml uploads as perf-history-<runner>.
module.exports = async ({ github, context, core }) => {
  const repo = { owner: context.repo.owner, repo: context.repo.repo };
  const name = `perf-history-${process.env.RUNNER_NAME}`;
  const current = (await github.rest.actions.getWorkflowRun({ ...repo, run_id: context.runId })).data;
  const runs = (await github.rest.actions.listWorkflowRuns({
    ...repo,
    workflow_id: 'performance.yml',
    branch: 'main',
    event: 'schedule',
    status: 'completed',
    per_page: 100,
  })).data.workflow_runs.filter((run) => run.id != current.id && run.created_at < current.created_at);
  for (const run of runs) {
    const artifacts = await github.rest.actions.listWorkflowRunArtifacts({ ...repo, run_id: run.id, name });
    if (artifacts.data.artifacts.some((a) => !a.expired)) {
      core.setOutput('run_id', String(run.id));
      return;
    }
  }
  core.warning(`No nightly of main before this run kept ${name}: no Last Run.`);
  core.setOutput('run_id', '');
};
