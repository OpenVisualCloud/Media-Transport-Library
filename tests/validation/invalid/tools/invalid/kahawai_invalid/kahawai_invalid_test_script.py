import os
import platform
import shutil
import subprocess
import sys
from multiprocessing import Process

from bind_network_card import bind_card
from modify_json_file_script import init_json_file, modify_json_file
from result_keyword_library import get_keyword

default_path = os.getcwd()
if platform.system().lower() == "windows":
    slash = "\\"
    work_path = r"C:\ws\workspace"
    test_path = work_path + r"\libraries.media.st2110.kahawai\app\build"
    build_path = work_path + r"\libraries.media.st2110.kahawai"
    yuv_path = r"Z:\kahawai\yuvs"
    mount_cmd = "net use z: \\10.67.116.200\\datadisk\\streams intel123 /user:media"
    nic_port_list = ["0000:b1:00.0", "0000:b1:00.1"]
    cmd_prefix = r"start /Node 1 /B .\RxTxApp.exe "
else:
    slash = "/"
    work_path = "/home/media/ws/workspace"
    test_path = work_path + "/libraries.media.st2110.kahawai/build/app"
    build_path = work_path + "/libraries.media.st2110.kahawai"
    yuv_path = "/home/media/ws/yuvs_do_not_delete"
    dpdk_script = "/home/media/ws/workspace/dpdk/usertools/dpdk-devbind.py"
    bind_script = (
        "/home/media/ws/workspace/libraries.media.st2110.kahawai/script/nicctl.sh"
    )
    mount_cmd = "sudo mount -o vers=3 10.67.116.200:/datadisk/streams/kahawai/yuvs /home/media/ws/yuvs_do_not_delete"
    nic_port_list, dma_port_list = bind_card(dpdk_script, bind_script)
    print(nic_port_list)
    cmd_prefix = "sudo ./RxTxApp "

log_path = default_path + slash + "logs"
per_case_log_path = log_path + slash + "case_log"
result_log_path = log_path + slash + "kahawai_invalid_test"

default_yuv_name = (
    "Netflix_Crosswalk_1920x1080_10bit_60Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv"
)
rxtx_default_json_file = "1080p59_1v_slice.json"
rx_default_json_file = "test_rx_1port_1v.json"
tx_default_json_file = "test_tx_1port_1v.json"
yuv = yuv_path + slash + default_yuv_name


if os.path.exists(yuv):
    print("The yuvs library has been mounted")
else:
    if platform.system().lower() == "windows":
        print("Mount the yuvs library")
        os.system(mount_cmd)
    else:
        if os.path.exists(yuv_path):
            print("Mount the yuvs library")
            os.system(mount_cmd)
        else:
            print("Make the yuv path and Mount the yuvs library")
            os.mkdir(yuv_path)
            os.system(mount_cmd)


suite_name = sys.argv[1]
test_mode = suite_name.split("-")[0]
parameter = suite_name.split("-")[1]
value_list = suite_name.split("-")[2:]
value_list.reverse()


json_file_mode_list = ["1_json_file", "2_json_file"]
port_dict = dict(p_port=nic_port_list[0], r_port=nic_port_list[1])
ip_dict = dict(
    tx_interfaces="192.168.17.101",
    rx_interfaces="192.168.17.102",
    tx_sessions="239.168.84.1",
    rx_sessions="239.168.84.1",
)


if test_mode == "wrong_value_in_json" and parameter == "video_url":
    yuv_dict = dict(
        tx_i720p59_yuv="ParkJoy_1280x720_10bit_50Hz_P422_To_yuv422p10be_To_yuv422YCBCR10be.yuv",
        tx_1080p_h264="H264_1920x1080_walking_couple_25Hz_main_IBBP_f200_29M.h264",
        tx_txt_file="test.txt",
        tx_nonexistent_yuv="nonexistent.yuv",
    )
    yuv = yuv_path + slash + yuv_dict[value_list[0]]


def gather_result_log():
    if json_file_mode == "1_json_file":
        os.rename(
            per_case_log_path + slash + log_file_name,
            per_case_log_path + slash + final_log_name,
        )
    else:
        with open(
            per_case_log_path + slash + final_log_name, "a"
        ) as final_log_name_handle_a:
            with open(
                per_case_log_path + slash + tx_log_file_name, "r"
            ) as tx_log_file_name_handle_r:
                for line in tx_log_file_name_handle_r.readlines():
                    final_log_name_handle_a.write(line)
            final_log_name_handle_a.write(
                "+++++++++++++++++++++++++++++Below is Rx logs+++++++++++++++++++\n"
            )
            with open(
                per_case_log_path + slash + rx_log_file_name, "r"
            ) as rx_json_file_name_handle_r:
                for line in rx_json_file_name_handle_r.readlines():
                    final_log_name_handle_a.write(line)


def check_test_result():
    if (
        test_mode == "wrong_value_in_json"
        and parameter == "video_url"
        and value_list[0] == "tx_nonexistent_yuv"
    ):
        result_keyword = get_keyword(test_mode, "video_url_nonexistent")
    elif (
        test_mode == "wrong_value_in_json"
        and parameter == "video_url"
        and value_list[0] == "tx_txt_file"
    ):
        result_keyword = get_keyword(test_mode, "video_url_txt_file")
    elif (
        test_mode == "wrong_value_in_command"
        and parameter == "config_file"
        and "config.txt" in value_list[0]
    ):
        result_keyword = get_keyword(test_mode, "config_file_txt")
    else:
        result_keyword = get_keyword(test_mode, parameter)
    if json_file_mode == "1_json_file":
        test_result = "none"
        with open(per_case_log_path + slash + log_file_name, "r") as log_file_handle_r:
            for line in log_file_handle_r.readlines():
                if result_keyword in line:
                    test_result = "pass"
                    print(line)
                    break
                else:
                    continue
        if test_result == "pass":
            final_result = "pass"
        else:
            final_result = "failed"
        with open(
            result_log_path + slash + "kahawai_invalid_test_result.log", "a"
        ) as result_log_handle_a:
            result_log_handle_a.write(
                suite_name + "-1_json_file" + ":" + final_result + "\n"
            )
    else:
        rx_test_result = "none"
        tx_test_result = "none"
        with open(
            per_case_log_path + slash + tx_log_file_name, "r"
        ) as tx_log_file_handle_r:
            for line in tx_log_file_handle_r.readlines():
                if result_keyword in line:
                    tx_test_result = "pass"
                    print(line)
                    break
                else:
                    continue
        with open(
            per_case_log_path + slash + rx_log_file_name, "r"
        ) as rx_log_file_handle_r:
            for line in rx_log_file_handle_r.readlines():
                if result_keyword in line:
                    rx_test_result = "pass"
                    print(line)
                    break
                else:
                    continue
        if tx_test_result == "pass" or rx_test_result == "pass":
            final_result = "pass"
        else:
            final_result = "failed"
        with open(
            result_log_path + slash + "kahawai_invalid_test_result.log", "a"
        ) as result_log_handle_a:
            result_log_handle_a.write(
                suite_name + "-2_json_file" + ":" + final_result + "\n"
            )
    print(final_result)


def run_test(command_line):
    print(command_line)
    os.chdir(test_path)
    p = subprocess.Popen(command_line, stdout=subprocess.PIPE, shell=True)
    p.communicate()


def prepare_json_test_command():
    test_command_list = []
    if json_file_mode == "1_json_file":
        shutil.move(json_file_name, per_case_log_path)
        test_command = (
            cmd_prefix
            + "--config_file "
            + per_case_log_path
            + slash
            + json_file_name
            + " --test_time 60 > "
            + per_case_log_path
            + slash
            + log_file_name
            + " 2>&1"
        )
        test_command_list.append(test_command)
        return test_command_list
    else:
        shutil.move(tx_json_file_name, per_case_log_path)
        shutil.move(rx_json_file_name, per_case_log_path)
        tx_test_command = (
            cmd_prefix
            + "--config_file "
            + per_case_log_path
            + slash
            + tx_json_file_name
            + " --test_time 60 > "
            + per_case_log_path
            + slash
            + tx_log_file_name
            + " 2>&1"
        )
        rx_test_command = (
            cmd_prefix
            + "--config_file "
            + per_case_log_path
            + slash
            + rx_json_file_name
            + " --test_time 60 > "
            + per_case_log_path
            + slash
            + rx_log_file_name
            + " 2>&1"
        )
        test_command_list.append(tx_test_command)
        test_command_list.append(rx_test_command)
        return test_command_list


def prepare_commandline_test_command():
    test_command_list = []
    tmp_test_command = ""
    shutil.move(json_file_name, per_case_log_path)
    init_command = (
        cmd_prefix
        + "--config_file "
        + per_case_log_path
        + slash
        + json_file_name
        + " --test_time 60"
    )
    if "parameter" in test_mode:
        if parameter in ["test_time"]:
            init_command = init_command.replace("_rxtx.json", ".json")
        if "--" + parameter in init_command:
            test_command = (
                init_command.replace("--" + parameter, "--" + value_list[0])
                + " > "
                + per_case_log_path
                + slash
                + log_file_name
                + " 2>&1"
            )
            test_command_list.append(test_command)
        else:
            if len(value_list) == 1:
                test_command = (
                    init_command
                    + " --"
                    + value_list[0]
                    + " > "
                    + per_case_log_path
                    + slash
                    + log_file_name
                    + " 2>&1"
                )
            else:
                test_command = (
                    init_command
                    + " --"
                    + value_list[1]
                    + " "
                    + value_list[0]
                    + " > "
                    + per_case_log_path
                    + slash
                    + log_file_name
                    + " 2>&1"
                )
            test_command_list.append(test_command)
    else:
        if parameter == "config_file" and value_list[0] == "config.txt":
            tmp_json_file_name = per_case_log_path + slash + value_list[0]
            os.rename(per_case_log_path + slash + json_file_name, tmp_json_file_name)
            value_list[0] = tmp_json_file_name

        for line in init_command.split("--")[1:]:
            if parameter + " " in line:
                tmp_test_command = (
                    tmp_test_command + "--" + parameter + " " + value_list[0] + " "
                )
                continue
            tmp_test_command = tmp_test_command + "--" + line + " "
        test_command = (
            init_command.split("--")[0]
            + tmp_test_command
            + " > "
            + per_case_log_path
            + slash
            + log_file_name
            + " 2>&1"
        )
        test_command_list.append(test_command)
    return test_command_list


# Prepare the test environment
def prepare_test_env():
    # Remove the old logs path
    if os.path.exists(log_path):
        print("Remove the old logs folder")
        shutil.rmtree(log_path)
        print("Make the logs path")
        os.makedirs(per_case_log_path)
        os.makedirs(result_log_path)
    else:
        print("Not exist logs path, make them")
        os.makedirs(per_case_log_path)
        os.makedirs(result_log_path)


if __name__ == "__main__":
    # Prepare test environment and sample json file
    prepare_test_env()
    for json_file_mode in json_file_mode_list:
        json_file_list = []
        rxtx_process_list = []
        if json_file_mode == "1_json_file":
            json_file_name = suite_name + "-1_json_file_rxtx.json"
            log_file_name = suite_name + "-1_json_file_rxtx.log"
            final_log_name = suite_name + "-1_json_file.log"
            sample_json_file = (
                build_path
                + slash
                + "tests"
                + slash
                + "script"
                + slash
                + rxtx_default_json_file
            )
            shutil.copy(sample_json_file, json_file_name)
            json_file_list.append(json_file_name)
        else:
            if "command" in test_mode:
                break
            else:
                rx_json_file_name = suite_name + "-2_json_file_rx.json"
                tx_json_file_name = suite_name + "-2_json_file_tx.json"
                rx_log_file_name = suite_name + "-2_json_file_rx.log"
                tx_log_file_name = suite_name + "-2_json_file_tx.log"
                final_log_name = suite_name + "-2_json_file.log"
                json_file_list.append(tx_json_file_name)
                json_file_list.append(rx_json_file_name)
                rx_sample_json_file = (
                    build_path + slash + "config" + slash + rx_default_json_file
                )
                tx_sample_json_file = (
                    build_path + slash + "config" + slash + tx_default_json_file
                )
                shutil.copy(rx_sample_json_file, rx_json_file_name)
                shutil.copy(tx_sample_json_file, tx_json_file_name)
        # Modify json file

        if "command" in test_mode:
            init_json_file(json_file_list, json_file_mode, port_dict, ip_dict, yuv)
            test_command_list_g = prepare_commandline_test_command()
        else:
            init_json_file(json_file_list, json_file_mode, port_dict, ip_dict, yuv)
            if test_mode == "wrong_value_in_json" and parameter == "video_url":
                pass
            else:
                modify_json_file(
                    json_file_list, json_file_mode, test_mode, parameter, value_list
                )
            test_command_list_g = prepare_json_test_command()
        print(
            "----------------------------%s--------------------------------"
            % json_file_mode
        )
        # Execute test
        for test_command_g in test_command_list_g:
            rxtx_process = Process(target=run_test, args=(test_command_g,))
            rxtx_process.start()
            rxtx_process_list.append(rxtx_process)
        for rxtx_process_tag in rxtx_process_list:
            rxtx_process_tag.join()

        check_test_result()
        gather_result_log()
