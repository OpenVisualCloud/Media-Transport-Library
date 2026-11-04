'use strict';

// The baseline of each suite for the combined report of this nightly run: the last
// scheduled run of main before this one that kept the reports of the suite. A branch
// dispatch is no baseline for main, and a run without reports compares nothing.
module.exports = async ({ github, context, core }) => {
  const repo = { owner: context.repo.owner, repo: context.repo.repo };
  const current = (await github.rest.actions.getWorkflowRun({ ...repo, run_id: context.runId })).data;
  const runs = (await github.rest.actions.listWorkflowRuns({
    ...repo,
    workflow_id: 'nightly.yml',
    branch: 'main',
    event: 'schedule',
    per_page: 10,
    status: 'completed',
  })).data.workflow_runs.filter((run) => run.id != current.id && run.created_at < current.created_at);

  async function findBaseline(reportPrefix) {
    for (const run of runs) {
      const artifacts = await github.rest.actions.listWorkflowRunArtifacts({ ...repo, run_id: run.id, per_page: 100 });
      if (artifacts.data.artifacts.some((a) => !a.expired && a.name.startsWith(reportPrefix))) return run.id;
    }
    return '';
  }

  core.setOutput('baseline_pytest_run_id', String(await findBaseline('nightly-test-report-')));
  core.setOutput('baseline_gtest_run_id', String(await findBaseline('nightly-gtest-report-')));
};
