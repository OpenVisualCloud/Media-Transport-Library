'use strict';

module.exports = async ({ github, context, core }) => {
  async function findRun(workflow, branch, runNumber) {
    const params = {
      owner: context.repo.owner,
      repo: context.repo.repo,
      workflow_id: `${workflow}.yml`,
      per_page: runNumber ? 100 : 1,
      status: 'completed',
    };
    if (branch && !runNumber) params.branch = branch;
    const runs = await github.rest.actions.listWorkflowRuns(params);
    return runNumber
      ? runs.data.workflow_runs.find((run) => run.run_number == runNumber) || null
      : runs.data.workflow_runs[0] || null;
  }

  // The last scheduled run of main before the current one that kept its reports: a
  // branch dispatch is no baseline for main, and a run without reports compares nothing.
  async function findBaseline(workflow, reportPrefix, explicitRunNumber, currentRun) {
    if (explicitRunNumber) {
      const run = await findRun(workflow, '', explicitRunNumber);
      return run ? run.id : '';
    }
    const runs = await github.rest.actions.listWorkflowRuns({
      owner: context.repo.owner,
      repo: context.repo.repo,
      workflow_id: `${workflow}.yml`,
      branch: 'main',
      event: 'schedule',
      per_page: 10,
      status: 'completed',
    });
    for (const run of runs.data.workflow_runs) {
      if (currentRun && (run.id == currentRun.id || run.created_at >= currentRun.created_at)) continue;
      const artifacts = await github.rest.actions.listWorkflowRunArtifacts({
        owner: context.repo.owner,
        repo: context.repo.repo,
        run_id: run.id,
        per_page: 100,
      });
      if (artifacts.data.artifacts.some((a) => !a.expired && a.name.startsWith(reportPrefix))) return run.id;
    }
    return '';
  }

  const pytestRun = await findRun('nightly-pytest', process.env.PYTEST_BRANCH, process.env.PYTEST_RUN_NUMBER);
  const gtestRun = await findRun('nightly-gtest', process.env.GTEST_BRANCH, process.env.GTEST_RUN_NUMBER);
  core.setOutput('pytest_run_id', pytestRun?.id || '');
  core.setOutput('gtest_run_id', gtestRun?.id || '');
  core.setOutput('baseline_pytest_run_id', String(await findBaseline(
    'nightly-pytest', 'nightly-test-report-', process.env.BASELINE_PYTEST_RUN_NUMBER, pytestRun,
  )));
  core.setOutput('baseline_gtest_run_id', String(await findBaseline(
    'nightly-gtest', 'nightly-gtest-report-', process.env.BASELINE_GTEST_RUN_NUMBER, gtestRun,
  )));
};
