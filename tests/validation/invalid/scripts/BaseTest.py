import os
import re
import sys

import yaml


class BaseTest(object):
    def __init__(self, command, config_file_path=None, expect_result=None):  
        self.command = command
        self.expect_result = expect_result
        self.config_file_path = config_file_path
        self.log_file_path = os.path.join(os.getcwd(), 'logs/result.log')
        self.output_log_path = os.path.join(os.getcwd(), 'logs/output.log')
        self.configs = self.parse_config()

    def pre_action(self):
        print "pre action"

    def post_action(self):
        print "post action"

    def parse_config(self):
        res = None
        print(self.config_file_path)
        if self.config_file_path and os.path.exists(self.config_file_path):
            f = open(self.config_file_path, 'r')
            res = yaml.load(f)
        return res

    def parse_and_store_result(self, output_log_path):
        with open(output_log_path, 'r') as f:
            lines = f.read()
            retcode = re.match(r'^retcode: (\d+)', lines).group(1)
            print "re_result:"
            print lines
            print retcode
            result = 'FAILED'
            if retcode == '0' and self.expect_result != 'null' and self.expect_result in lines:
                result = 'PASSED'
            if retcode == '0' and self.expect_result == 'null':
                result = 'PASSED'
            self.store_case_result(self.command.replace(' ', '_'), result)

    def store_case_result(self, case_name, result):
        with open(self.log_file_path, 'a+') as f:
            f.write('case_name: %s\n' % case_name)
            f.write(result + '\n')
        f.close()

    def test(self):
        print("_________________self.command____________-----")
        print self.command
        retcode = os.system(self.command)
        output = os.popen(self.command).read()

        if os.path.exists(self.log_file_path):
            os.remove(self.log_file_path)
        f1 = open(self.output_log_path, 'w')
        f1.write('retcode: %s\n' % retcode)
        f1.write(output)
        f1.close()
        self.parse_and_store_result(self.output_log_path)

    def run(self):
        try:
            self.pre_action()
            self.test()
            self.post_action()
        finally:
            self.stop_aic_after_test()

    @staticmethod
    def stop_aic_after_test():
        print("stop action")
        '''
        #need delete by caixuan
        stop_all_script = 'tools/common/stop_all.sh'
        command = 'bash {}'.format(os.path.join(os.getcwd(), stop_all_script))
        print('stop all: {}'.format(command))
        os.system(command)
        '''


def running_test(command, expect_result):
    base = BaseTest(command=command, expect_result=expect_result)
    base.run()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print 'Error: BaseScript params missing'
    else:
        base = BaseTest(command=' '.join(sys.argv[1:-1]), expect_result=sys.argv[-1])
        base.run()





