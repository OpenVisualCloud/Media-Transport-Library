import sys
import os
import importlib
import getopt
import platform

if platform.system() == "Linux":
    slash = "/"
else:
    slash = "\\"


class Exect(object):
    scripts_path = 'scripts' + slash

    def __init__(self, params):
        self.params = params
        self.test_type = None
        self.test_cmd = None
        self.expect_result = None
        self.exec_script_path = None

    def locate_test_script(self):
        script_path = os.path.join(self.scripts_path, 'BaseTest.py')
        for dirpath, _, filenames in os.walk(self.scripts_path):
            for file in filenames:
                if self.test_type == os.path.splitext(file)[0]:
                    script_path = os.path.join(dirpath, file)
                    break
        return script_path

    def parse_params(self):
        # single cmd:  -t cmd -c command -er expected_result
        # one identify test:  -t test_type -c command
        # full test: -t test_type
        options = "ht:c:er:"
        options_long = ["help", "test_type=", "command=", "expected_result="]

        try:
            opts, args = getopt.getopt(self.params, options, options_long)
            if len(opts) < 1:
                raise getopt.GetoptError('Missing input args.')

            for opt, arg in opts:
                if opt in ("-h", "--help"):
                    raise getopt.GetoptError('')
                if opt in ("-t", "--test_type"):
                    self.test_type = arg
                elif opt in ("-c", "--command"):
                    self.test_cmd = arg
                elif opt in ("-er", "--expected_result"):
                    self.expect_result = arg
            if not self.expect_result:
                self.expect_result = 'null'
        except Exception as ex:
            #print ex.msg
            print(("Usage: \n" 
                "simple cmd with expected_result: python Execut.py -t cmd -c 'test_command' -er 'expected_result'\n" 
                "simple cmd: python Execut.py -t cmd -c 'test_command'\n" 
                "one specific test of test type: python Execut.py -t 'test_type' -c 'test_command'\n" 
                "full test of test type : python Execut.py -t 'test_type'\n"))
            sys.exit(1)

        print(self.test_type, self.test_cmd, self.expect_result)
        return

    def exec_test(self):
        self.parse_params()
        self.exec_script_path = self.locate_test_script()
        print(self.exec_script_path)
        print(self.test_type)
        if not self.exec_script_path:
            print ("Error: no test script match!")
            return
        # sys.path.append('/home/media/ws/vcaa_test/scripts')
        print ("expect_result: %s" % self.expect_result)
        if self.test_type == 'cmd':
            test_module = importlib.import_module("scripts.BaseTest")
            test_module.running_test(command=self.test_cmd, expect_result=self.expect_result)
        else:
            test_module = importlib.import_module("scripts.%s" % self.test_type)
            test_module.running_test(command=self.test_cmd)


if __name__ == "__main__":
    input_params = sys.argv[1:]
    exect = Exect(params=input_params)
    exect.exec_test()
