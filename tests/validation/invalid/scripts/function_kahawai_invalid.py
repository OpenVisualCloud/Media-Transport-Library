import os
import platform
from BaseTest import BaseTest

if platform.system() == "Linux":
    slash = "/"
else:
    slash = "\\"

yaml_file = 'configs' + slash + 'kahawai_invalid_test.yaml'
config_file_path = os.path.join(os.getcwd(), yaml_file)
running_test_path = 'tools' + slash + 'invalid' + slash + 'kahawai_invalid'
log_file = 'logs' + slash + 'kahawai_invalid_test' + slash + 'kahawai_invalid_test_result.log'


class ConnTest(BaseTest):
    def __init__(self, command=None):
        BaseTest.__init__(self, command, config_file_path, None)
        self.command = command
        print("cmd: %s" % self.command)
        print(self.configs)

    def pre_action(self):
        print("pre action")
        pre_shell = self.configs['pre_action']['shell']
        if pre_shell:
            os.system('bash %s' % pre_shell)

    def post_action(self):
        print "post action"
        post_shell = self.configs['post_action']['shell']
        if post_shell:
            os.system('bash %s' % post_shell)

    def parse_and_store_result(self, output_log_path):
        print 'output log path: %s' % output_log_path
        with open(output_log_path, 'r') as f:
            lines = f.read().splitlines()
            for line in lines:
                case_name=line.split(':')[0]
                if line.split(':')[1] == 'pass':
                    result = 'PASSED'
                elif line.split(':')[1] == 'failed':
                    result = 'FAILED'
                elif line.split(':')[1] == 'block':
                    result = 'BLOCKED'
                self.store_case_result(case_name, result)
        f.close()

    def test(self):
        shell_script = running_test_path + slash + self.configs['running_test']['shell']
        if self.command:
            print "Identify test"
            #self.command = 'bash {} {}'.format(os.path.join(os.getcwd(), shell_script),self.command)
            self.command = 'python {} {}'.format(os.path.join(os.getcwd(), shell_script), self.command)
        else:
            print "Full test"
            #self.command = 'bash %s ' % os.path.join(os.getcwd(), shell_script)
            self.command = 'python %s ' % os.path.join(os.getcwd(), shell_script)

        command = '%s' % self.command

        print('test_command: %s' % command)
        os.system(command)
        self.output_log_path=os.path.join(os.getcwd(), log_file)
        # f1 = open(self.output_log_path, 'a')
        # f1.write('retcode output: %s' % output)
        # f1.close()
        self.parse_and_store_result(self.output_log_path)


def running_test(command):
    print(command)
    conntest = ConnTest(command=command)
    conntest.run()
