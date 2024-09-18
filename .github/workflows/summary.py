import yaml

import yaml

# name
# triggers
# jobs names
# runs-on 
# jobs steps


with open("afxdp_build.yml") as stream:
    content = yaml.safe_load(stream)
    print([content["jobs"][job].keys() for job in content["jobs"]])