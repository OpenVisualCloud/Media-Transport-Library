'use strict';

// The baseline of each suite: the last nightly of main before this run that kept the
// suite's reports. A nightly is a cron run or a dispatch titled FULL_NIGHTLY (run-name).
const FULL_NIGHTLY = 'nightly main';

module.exports = async ({ github, context, core }) => {
  const repo = { owner: context.repo.owner, repo: context.repo.repo };
  const current = (await github.rest.actions.getWorkflowRun({ ...repo, run_id: context.runId })).data;
  const runs = (await github.rest.actions.listWorkflowRuns({
    ...repo,
    workflow_id: 'nightly.yml',
    branch: 'main',
    per_page: 100,
    status: 'completed',
  })).data.workflow_runs.filter((run) => run.id != current.id && run.created_at < current.created_at &&
    (run.event == 'schedule' || (run.event == 'workflow_dispatch' && run.display_title == FULL_NIGHTLY)));

  async function findBaseline(reportPrefix) {
    for (const run of runs) {
      const artifacts = await github.rest.actions.listWorkflowRunArtifacts({ ...repo, run_id: run.id, per_page: 100 });
      if (artifacts.data.artifacts.some((a) => !a.expired && a.name.startsWith(reportPrefix))) return run.id;
    }
    core.warning(`No nightly of main before this run kept ${reportPrefix}* reports: no baseline.`);
    return '';
  }

  core.setOutput('baseline_pytest_run_id', String(await findBaseline('nightly-test-report-')));
  core.setOutput('baseline_gtest_run_id', String(await findBaseline('nightly-gtest-report-')));
};
