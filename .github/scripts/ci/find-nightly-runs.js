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

  async function findBaseline(workflow, explicitRunNumber, currentRunId) {
    if (explicitRunNumber) {
      const run = await findRun(workflow, '', explicitRunNumber);
      return run ? run.id : '';
    }
    const runs = await github.rest.actions.listWorkflowRuns({
      owner: context.repo.owner,
      repo: context.repo.repo,
      workflow_id: `${workflow}.yml`,
      per_page: 5,
      status: 'completed',
    });
    return runs.data.workflow_runs.find((run) => run.id != currentRunId)?.id || '';
  }

  const pytestRun = await findRun('nightly-pytest', process.env.PYTEST_BRANCH, process.env.PYTEST_RUN_NUMBER);
  const gtestRun = await findRun('nightly-gtest', process.env.GTEST_BRANCH, process.env.GTEST_RUN_NUMBER);
  core.setOutput('pytest_run_id', pytestRun?.id || '');
  core.setOutput('gtest_run_id', gtestRun?.id || '');
  core.setOutput('baseline_pytest_run_id', String(await findBaseline(
    'nightly-pytest', process.env.BASELINE_PYTEST_RUN_NUMBER, pytestRun?.id || '',
  )));
  core.setOutput('baseline_gtest_run_id', String(await findBaseline(
    'nightly-gtest', process.env.BASELINE_GTEST_RUN_NUMBER, gtestRun?.id || '',
  )));
};