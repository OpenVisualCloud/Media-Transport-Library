'use strict';

module.exports = async ({ github, context, core }) => {
  const runId = Number(process.env.RUN_ID);
  const run = await github.rest.actions.getWorkflowRun({
    owner: context.repo.owner,
    repo: context.repo.repo,
    run_id: runId,
  });
  core.setOutput('run_date', run.data.created_at);
  core.setOutput('run_id', runId);
  core.setOutput('run_number', run.data.run_number);
  core.setOutput('branch', run.data.head_branch);
  core.setOutput('run_url', run.data.html_url);
  console.log(`${process.env.RUN_LABEL} Run #${run.data.run_number}, Branch: ${run.data.head_branch}`);
};
