import platform
import os
import sys
import subprocess
import shutil
from bind_network_card import bind_card 

os_format = platform.system()
suite_execute_name = sys.argv[1]
suite_name = suite_execute_name.split("-")[0]

cmd_prefix = ""

default_path = os.getcwd()

if os_format == "Windows":
    slash = "\\"
    call_cmd = ".\\"
    build_path = r"C:\ws\workspace\libraries.media.st2110.kahawai\tests\build"
    binary_name = "KahawaiTest.exe"
    cmd_prefix = "start /Node 0 /B "
    init_cmd = " --p_port 0000:b1:00.0 --r_port 0000:b1:00.1 "
    dma_port = "--dma_dev 0000:80:01.0,0000:80:01.7 "
else:
    slash = "/"
    script_suffix = ".sh"
    call_cmd = "./"
    build_path = "/home/gta/IMTL/Media-Transport-Library/build/tests"
    binary_name = "KahawaiTest"
    bind_binary = "/home/gta/IMTL/Media-Transport-Library/script/nicctl.sh"
    dpdk_binary = "/home/gta/IMTL/dpdk/usertools/dpdk-devbind.py"
    nic_port_list, dam_port_list = bind_card(dpdk_binary, bind_binary)
    #nic_port_list, dam_port_list = ['0000:4b:11.0', '0000:4b:11.1'], ['0000:00:01.0', '0000:00:01.1', '0000:00:01.2', '0000:00:01.3', '0000:00:01.4', '0000:00:01.5', '0000:00:01.6', '0000:00:01.7']
    cmd_prefix = ""
    init_cmd = " --p_port " + nic_port_list[0] + " --r_port " + nic_port_list[1] + " "
    dma_port = "--dma_dev " + dam_port_list[0] + "," + dam_port_list[1] + " "

if "-dma" in suite_execute_name:
    dma_cmd = dma_port
else:
    dma_cmd = ""
if "runtime" in suite_execute_name:
    runtime_cmd = "--auto_start_stop "
else:
    runtime_cmd = ""
if "all_test" in suite_execute_name:
    filter_cmd = ""
else:
    filter_cmd = "--gtest_filter=" + suite_name + ".* "

result_path = default_path + slash + "logs" + slash + "kahawai_API_test"
result_log_file = default_path + slash + "logs" + slash + "kahawai_API_test" + slash + "kahawai_API_test.log"
per_case_output_path = default_path + slash + "logs" + slash + "case_log"
suite_log_file = per_case_output_path + slash + suite_name + ".log"

check_cmd = (cmd_prefix + call_cmd + binary_name + init_cmd + "--gtest_list_tests" + " > " + per_case_output_path +
             slash + "case_list.log 2>&1")
run_cmd = (cmd_prefix + call_cmd + binary_name + init_cmd + filter_cmd + runtime_cmd + dma_cmd + "> " +
           suite_log_file + " 2>&1")

if os.path.exists("logs"):
    print("remove the logs folder")
    shutil.rmtree("logs")
print("make logs folder")
os.makedirs(result_path)
os.makedirs(per_case_output_path)


def check_test_env():
    os.chdir(build_path)
    case_list_log = per_case_output_path + slash + "case_list.log"
    print("------------------Checking the test environment------------------")
    if os.path.exists(binary_name):
        print("KahawaiTest test has been built successfully !!")
    else:
        print("Not find the KahawaiTest binary, please check it")
        sys.exit()

    print(check_cmd)
    check_binary_result = subprocess.Popen(check_cmd, stdout=subprocess.PIPE, shell=True)
    check_binary_result.communicate()
    print("------------------Get case list done------------------")
    with open(case_list_log, "r") as file_handle1:
        for string_index in file_handle1.readlines():
            if "st_init fail" in str(string_index):
                print("Get case list failed, please check the KahawaiTest binary")
                case_name = "check_test_env"
                case_log_file = per_case_output_path + slash + case_name + ".log"
                with open(case_log_file, 'w') as file_handle2:
                    file_handle2.write("Get case list failed, please check the KahawaiTest binary")
                with open(result_log_file, 'a') as file_handle3:
                    file_handle3.write(case_name + ":failed")
                sys.exit()
            else:
                continue


def run_test():
    os.chdir(build_path)
    print("------------------The %s suite is executing------------------" % suite_name)
    print(run_cmd)
    run_binary_result = subprocess.Popen(run_cmd, stdout=subprocess.PIPE, shell=True)
    run_binary_result.communicate()
    print("------------------The %s suite has been executed------------------" % suite_name)


def check_result():
    os.chdir(default_path)
    print("------------------Checking the %s test result------------------" % suite_name)
    write_tag = 0
    case_name = ""
    case_log_file = ""
    result = ""
    with open(suite_log_file, "r") as file_handle1:
        for log_index in file_handle1.readlines():
            if "RUN" in log_index:
                case_name = log_index.split("] ")[1].strip()
                case_log_name = case_name + ".log"
                case_log_file = per_case_output_path + slash + case_log_name
                write_tag = 1
            elif "OK" in log_index:
                write_tag = 2
                result = "pass"
            elif "FAILED" in log_index:
                write_tag = 2
                result = "failed"
            elif "Global test environment tear-down" in log_index:
                break
            if write_tag >= 1:
                with open(case_log_file, "a") as file_handle2:
                    file_handle2.write(log_index)
                if write_tag == 2:
                    with open(result_log_file, "a") as file_handle3:
                        file_handle3.write(case_name + ":" + result + "\n")
                    write_tag = 0


check_test_env()
run_test()
check_result()

