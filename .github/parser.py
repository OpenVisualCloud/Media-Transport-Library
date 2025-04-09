import os
import yaml
from yaml.constructor import Constructor

def add_bool(self, node):
    return self.construct_scalar(node)

Constructor.add_constructor(u'tag:yaml.org,2002:bool', add_bool)

workflows_dir = 'workflows'
output_file = 'workflows.txt'

def parse_workflow(file_path):
    with open(file_path, 'r') as file:
        data = yaml.safe_load(file)
        jobs = ', '.join(data.get('jobs', {}).keys())
        triggers = ', '.join(data.get(True, {}).keys())
        print(jobs,triggers)
        steps = ', '.join([step.get('name', 'unnamed') for job in data.get('jobs', {}).values() for step in job.get('steps', [])])
        artifacts = ', '.join([artifact.get('name', 'unnamed') for job in data.get('jobs', {}).values() for artifact in job.get('artifacts', [])])
        return jobs, triggers, steps, artifacts

with open(output_file, 'w') as file:
    file.write('| File Name | Job Names | Triggers | Steps | Artifacts |\n')
    file.write('|-----------|-----------|----------|-------|-----------|\n')
    for workflow_file in os.listdir(workflows_dir):
        if workflow_file.endswith('.yml') or workflow_file.endswith('.yaml'):
            file_path = os.path.join(workflows_dir, workflow_file)
            jobs, triggers, steps, artifacts = parse_workflow(file_path)
            file.write(f'| {workflow_file} | {jobs} | {triggers} | {steps} | {artifacts} |\n')
