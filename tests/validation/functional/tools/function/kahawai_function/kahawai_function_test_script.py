import json
import os
import platform
import shutil
import subprocess
import sys
from collections import OrderedDict
from multiprocessing import Process

from bind_network_card import bind_card
from check_yuv_mount_path import check_yuv_mount_path
from modify_json_file_script import modify_json_file as deep_modify_json_file

default_path = os.getcwd()

if platform.system().lower() == "windows":
    slash = "\\"
    build_path = r"C:\ws\workspace\libraries.media.st2110.kahawai"
    test_path = build_path + r"\app\build"
    yuv_path = r"Z:\kahawai\yuvs"
    init_cmd = r"start /Node 1 /B .\RxTxApp --config_file "
    nic_port_list = ['0000:b1:00.0', '0000:b1:00.1']
    dma_port_list = ['000:80:01.0','0000:80:01.1']
else:
    slash = "/"
    build_path = "/home/gta/IMTL/Media-Transport-Library"
    test_path = "/home/gta/IMTL/Media-Transport-Library/build/app"
    yuv_path = "/home/gta/IMTL/media"
    init_cmd = "sudo ./RxTxApp --config_file "
    dpdk_script = '/home/gta/IMTL/dpdk/usertools/dpdk-devbind.py'
    bind_script = '/home/gta/IMTL/Media-Transport-Library/script/nicctl.sh'
    #nic_port_list, dma_port_list = bind_card(dpdk_script, bind_script)
    nic_port_list = ['0000:4b:11.0', "0000:4b:11.1"] 
    #dma_port_list = [""]
    #nic_port_list, dam_port_list = ['0000:4b:11.0', '0000:4b:11.1'], ['0000:00:01.0', '0000:00:01.1', '0000:00:01.2', '0000:00:01.3', '0000:00:01.4', '0000:00:01.5', '0000:00:01.6', '0000:00:01.7']

result_file = default_path + slash + "logs" + slash + "kahawai_function_test" + slash + "kahawai_function_test.log"
per_case_output_path = default_path + slash + "logs" + slash + "case_log"

yuv_dict = dict(i1080p59='Netflix_Crosswalk_1920x1080_10bit_60Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i1080p50='ParkJoy_1920x1080_10bit_50Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i1080p29='Plalaedit_Pedestrian_10bit_1920x1080_30Hz_P420_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i1080p25='HDR_BBC_v4_008_Penguin1_1920x1080_10bit_25Hz_180frames_yuv422p10be_To_yuv422rfc4175be10.yuv',
                i1080p119='Netflix_Crosswalk_1920x1080_10bit_60Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i1080p60='Netflix_Crosswalk_1920x1080_10bit_60Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i1080p23='HDR_BBC_v4_008_Penguin1_1920x1080_10bit_25Hz_180frames_yuv422p10be_To_yuv422rfc4175be10.yuv',
                i1080p24='HDR_BBC_v4_008_Penguin1_1920x1080_10bit_25Hz_180frames_yuv422p10be_To_yuv422rfc4175be10.yuv',
                i1080p30='HDR_BBC_v4_008_Penguin1_1920x1080_10bit_25Hz_180frames_yuv422p10be_To_yuv422rfc4175be10.yuv',
                i720p25='HDR_BBC_v4_008_Penguin1_1280x720_10bit_25Hz_P422_180frames.yuv',
                i720p29='Plalaedit_Pedestrian_10bit_1280x720_30Hz_P420_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i720p50='ParkJoy_1280x720_10bit_50Hz_P422_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i720p59='Netflix_Crosswalk_1280x720_10bit_60Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i720p119='Netflix_Crosswalk_1280x720_10bit_60Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i720p60='Netflix_Crosswalk_1280x720_10bit_60Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i720p30='Plalaedit_Pedestrian_10bit_1280x720_30Hz_P420_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i720p23='HDR_BBC_v4_008_Penguin1_1280x720_10bit_25Hz_P422_180frames.yuv',
                i720p24='HDR_BBC_v4_008_Penguin1_1280x720_10bit_25Hz_P422_180frames.yuv',
                i2160p23='test_3840x2160_for_25fps.yuv',
                i2160p24='test_3840x2160_for_25fps.yuv',
                i2160p25='test_3840x2160_for_25fps.yuv',
                i2160p29='Plalaedit_Pedestrian_10bit_3840x2160_30Hz_P420_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i2160p30='Plalaedit_Pedestrian_10bit_3840x2160_30Hz_P420_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i2160p50='ParkJoy_3840x2160_10bit_50Hz_P422_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i2160p59='Netflix_Crosswalk_3840x2160_10bit_60Hz_P422_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i2160p60='Netflix_Crosswalk_3840x2160_10bit_60Hz_P422_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i2160p119='Netflix_Crosswalk_3840x2160_10bit_60Hz_P422_To_yuv422p10be_To_yuv422YCBCR10be.yuv',
                i4320p25='test_8k.yuv',
                i4320p29='test_8k.yuv',
                i4320p50='test_8k.yuv',
                i4320p59='test_8k.yuv',
                i4320p119='test_8k.yuv',
                i4320p60='test_8k.yuv',
                i4320p30='test_8k.yuv',
                i4320p24='test_8k.yuv',
                i4320p23='test_8k.yuv',
                i1080i50='ParkJoy_1920x1080_interlace_10bit_50Hz_P422.yuv',
                i1080i59='Netflix_Crosswalk_1920x1080_interlace_10bit_60Hz_P422.yuv',
                i480i59='Netflix_Crosswalk_720x480_interlace_10bit_60Hz_P422.yuv',
                i576i50='ParkJoy_720x576_interlace_10bit_50Hz_P422.yuv')


print("Checking yuv mount path...")
mount_code = check_yuv_mount_path()
if mount_code == 0:
    print("Monut Pass!!!")
else:
    print("Mount Fail, exits the script!!!!")
    sys.exit()

sample_json_file_dict = dict(i1080p59='1080p59_1v.json',
                             i1080p50='1080p50_1v.json',
                             i1080p29='1080p29_1v.json',
                             i1080p25='1080p25_1v.json',
                             i1080p119='1080p119_1v.json',
                             i1080p60='1080p60_1v.json',
                             i1080p30='1080p30_1v.json',
                             i1080p23='1080p30_1v.json',
                             i1080p24='1080p30_1v.json',
                             i720p25='720p25_1v.json',
                             i720p29='720p29_1v.json',
                             i720p50='720p50_1v.json',
                             i720p59='720p59_1v.json',
                             i720p119='720p59_1v.json',
                             i720p60='720p59_1v.json',
                             i720p30='720p59_1v.json',
                             i720p24='720p59_1v.json',
                             i720p23='720p59_1v.json',
                             i2160p25='4kp25_1v.json',
                             i2160p29='4kp29_1v.json',
                             i2160p50='4kp50_1v.json',
                             i2160p59='4kp59_1v.json',
                             i2160p119='4kp59_1v.json',
                             i2160p60='4kp59_1v.json',
                             i2160p30='4kp59_1v.json',
                             i2160p24='4kp59_1v.json',
                             i2160p23='4kp59_1v.json',
                             i4320p25='8kp59_1v.json',
                             i4320p29='8kp59_1v.json',
                             i4320p50='8kp59_1v.json',
                             i4320p59='8kp59_1v.json',
                             i4320p119='8kp59_1v.json',
                             i4320p60='8kp59_1v.json',
                             i4320p30='8kp59_1v.json',
                             i4320p24='8kp59_1v.json',
                             i4320p23='8kp59_1v.json',
                             i1080i50='1080i59_1v.json',
                             i1080i59='1080i59_1v.json',
                             i480i59='1080i59_1v.json',
                             i576i50='1080i59_1v.json')

port_dict = dict(p_port=nic_port_list[0], r_port=nic_port_list[1])

unicast_ip_dict = dict(tx_interfaces='192.168.17.101',
                       rx_interfaces='192.168.17.102',
                       tx_sessions='192.168.17.102',
                       rx_sessions='192.168.17.101')

multicast_ip_dict = dict(tx_interfaces='192.168.17.101',
                         rx_interfaces='192.168.17.102',
                         tx_sessions='239.168.48.9',
                         rx_sessions='239.168.48.9')

test_mode_list = ["unicast", "multicast"]
type_mode_list = ["frame", "rtp"]
#type_mode_list = ["frame",  "rtp", "slice"]


def check_test_result():
    os.chdir(default_path)
    print("++++++++++++++++++++++++++Checking the test result+++++++++++++++++++++++++++")
    if json_file_mode == "1_json_file":
        result_log = per_case_output_path + slash + case_name + ".log"
    else:
        tx_result_log = per_case_output_path + slash + str(os.path.splitext(json_file_list[0])[0]) + ".log"
        rx_result_log = per_case_output_path + slash + str(os.path.splitext(json_file_list[1])[0]) + ".log"
        result_log = per_case_output_path + slash + case_name + ".log"
        with open(tx_result_log, "r") as tx_result_log_handle:
            tx_result_content = tx_result_log_handle.read()
        with open(rx_result_log, "r") as rx_result_log_handle:
            rx_result_content = rx_result_log_handle.read()
        with open(result_log, "a") as result_log_handle:
            result_log_handle.write("+++++++++++++++++++++++tx test result++++++++++++++++\n")
            result_log_handle.write(tx_result_content)
            result_log_handle.write("+++++++++++++++++++++++rx test result++++++++++++++++\n")
            result_log_handle.write(rx_result_content)

    with open(result_log, "r") as result_log_handle_r:
        rx_result = ""
        tx_result = ""
        for line in result_log_handle_r.readlines():
            if "app_rx_video_result" in line:
                if "OK" in line:
                    print(line)
                    rx_result = "pass"
                else:
                    rx_result = "failed"
            elif "app_tx_video_result" in line:
                if "OK" in line:
                    print(line)
                    tx_result = "pass"
                else:
                    tx_result = "failed"
            else:
                continue
    if tx_result == "pass" and rx_result == "pass":
        result = "pass"
    else:
        result = "failed"
    with open(result_file, "a") as result_file_handle:
        result_file_handle.write(case_name + ":" + result + "\n")


def run_test(json_file_name, mode):
    os.chdir(test_path)
    if mode == 'tx':
        cmd = (init_cmd + per_case_output_path + slash + json_file_name + " --test_time " + str(test_time) + 
                " > " + per_case_output_path + slash + os.path.splitext(json_file_name)[0] + ".log 2>&1")
    else:
        cmd = (init_cmd + per_case_output_path + slash + json_file_name + dma_cmd +  " --test_time " + str(test_time) +
                " > " + per_case_output_path + slash + os.path.splitext(json_file_name)[0] + ".log 2>&1")
    print(cmd)
    rxtx_process = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
    rxtx_process.communicate()


def modify_json_file():
    os.chdir(per_case_output_path)
    if test_mode == "unicast":
        tx_interfaces_ip = unicast_ip_dict['tx_interfaces']
        rx_interfaces_ip = unicast_ip_dict['rx_interfaces']
        tx_sessions_ip = unicast_ip_dict['tx_sessions']
        rx_sessions_ip = unicast_ip_dict['rx_sessions']
    else:
        tx_interfaces_ip = multicast_ip_dict['tx_interfaces']
        rx_interfaces_ip = multicast_ip_dict['rx_interfaces']
        tx_sessions_ip = multicast_ip_dict['tx_sessions']
        rx_sessions_ip = multicast_ip_dict['rx_sessions']

    if json_file_mode == "1_json_file":
        with open(json_file_list[0], "r") as json_file_handle_r:
            json_content_dict = json.load(json_file_handle_r, object_pairs_hook=OrderedDict)
        json_content_dict['interfaces'][0]['ip'] = tx_interfaces_ip
        json_content_dict['interfaces'][1]['ip'] = rx_interfaces_ip
        json_content_dict['interfaces'][0]['name'] = port_dict['p_port']
        json_content_dict['interfaces'][1]['name'] = port_dict['r_port']
        json_content_dict['tx_sessions'][0]['dip'][0]= tx_sessions_ip
        json_content_dict['rx_sessions'][0]['ip'][0] = rx_sessions_ip
        json_content_dict['tx_sessions'][0]['video'][0]['type'] = type_mode
        json_content_dict['rx_sessions'][0]['video'][0]['type'] = type_mode
        json_content_dict['tx_sessions'][0]['video'][0]['video_format'] = video_format
        json_content_dict['rx_sessions'][0]['video'][0]['video_format'] = video_format
        json_content_dict['tx_sessions'][0]['video'][0]['video_url'] = yuv_path + slash + yuv_dict[video_format]

        with open(json_file_list[0], "w") as json_file_handle_w:
            json.dump(json_content_dict, json_file_handle_w, indent=4)
    else:
        with open(json_file_list[0], "r") as tx_json_file_handle_r:
            tx_json_content_dict = json.load(tx_json_file_handle_r, object_pairs_hook=OrderedDict)
        with open(json_file_list[1], "r") as rx_json_file_handle_r:
            rx_json_content_dict = json.load(rx_json_file_handle_r, object_pairs_hook=OrderedDict)
        tx_json_content_dict['interfaces'][0]['ip'] = tx_interfaces_ip
        rx_json_content_dict['interfaces'][0]['ip'] = rx_interfaces_ip
        tx_json_content_dict['interfaces'][0]['name'] = port_dict['p_port']
        rx_json_content_dict['interfaces'][0]['name'] = port_dict['r_port']
        tx_json_content_dict['tx_sessions'][0]['dip'][0]= tx_sessions_ip
        rx_json_content_dict['rx_sessions'][0]['ip'][0] = rx_sessions_ip
        tx_json_content_dict['tx_sessions'][0]['video'][0]['type'] = type_mode
        rx_json_content_dict['rx_sessions'][0]['video'][0]['type'] = type_mode
        tx_json_content_dict['tx_sessions'][0]['video'][0]['video_format'] = video_format
        rx_json_content_dict['rx_sessions'][0]['video'][0]['video_format'] = video_format
        tx_json_content_dict['tx_sessions'][0]['video'][0]['video_url'] = yuv_path + slash + yuv_dict[video_format]

        with open(json_file_list[0], "w") as tx_json_file_handle_w:
            json.dump(tx_json_content_dict, tx_json_file_handle_w, indent=4)
        with open(json_file_list[1], "w") as rx_json_file_handle_w:
            json.dump(rx_json_content_dict, rx_json_file_handle_w, indent=4)
    os.chdir(default_path)


def copy_json_file():
    if json_file_mode == "1_json_file":
        scr_json = build_path + slash + "tests" + slash + "script" + slash + "loop_json" + slash + sample_json_file_dict[video_format]
        if os.path.exists(scr_json):
            json_file_name = json_file_list[0]
            dst_json = per_case_output_path + slash + json_file_name
            shutil.copyfile(scr_json, dst_json)
        else:
            print('The "%s" file is not exist' % scr_json)
            sys.exit(0)
    else:
        tx_scr_json = build_path + slash + "config" + slash + "test_tx_1port_1v.json"
        rx_scr_json = build_path + slash + "config" + slash + "test_rx_1port_1v.json"
        if os.path.exists(tx_scr_json) and os.path.exists(rx_scr_json):
            tx_json_file_name = json_file_list[0]
            rx_json_file_name = json_file_list[1]
            tx_dst_json = per_case_output_path + slash + tx_json_file_name
            rx_dst_json = per_case_output_path + slash + rx_json_file_name
            shutil.copyfile(tx_scr_json, tx_dst_json)
            shutil.copyfile(rx_scr_json, rx_dst_json)

        else:
            print('The "test_tx_1port_1v.json" or "test_rx_1port_1v.json" is not exist')
            sys.exit(0)


suite_name = sys.argv[1]
print("SUITE NAME: ", suite_name)
video_format = suite_name.split("-")[0]
json_file_mode = suite_name.split("-")[1]
test_time = suite_name.split("-")[2]
yuv_file = yuv_path + slash + yuv_dict[video_format]
dma_tag = suite_name.split("-")[3]
parameter_name = suite_name.split("-")[4]
if parameter_name == "default":
    print("Execute the test case with default configuration")
    parameter_value_list = ['value']
else:
    parameter_value_list = suite_name.split("-")[5:]

if dma_tag == 'dma':
    dma_cmd = ' --dma_dev ' + dma_port_list[0] + "," + dma_port_list[1]
else:
    dma_cmd =''


if __name__ == '__main__':
    if os.path.exists(per_case_output_path):
        print("Remove logs")
        shutil.rmtree(default_path + slash + "logs")
        os.makedirs(per_case_output_path)
        os.makedirs(default_path + slash + "logs" + slash + "kahawai_function_test")
    else:
        print("logs path doesn't exist, make them")
        os.makedirs(per_case_output_path)
        os.makedirs(default_path + slash + "logs" + slash + "kahawai_function_test")

    for parameter_value in parameter_value_list:
        for test_mode in test_mode_list:
            for type_mode in type_mode_list:
                if '4320' in video_format and type_mode != "frame":
                    print("8K don't need to execut %s" %(type_mode))
                    continue
                case_name = (video_format + "_" + json_file_mode + "_" + test_mode + "_" + type_mode + "_" + 
                             parameter_name + "_" + parameter_value)
                if json_file_mode == "1_json_file":
                    json_file_name_global = case_name + ".json"
                    json_file_list = [json_file_name_global]
                else:
                    tx_json_file_name_global = case_name + "_tx.json"
                    rx_json_file_name_global = case_name + "_rx.json"
                    json_file_list = [tx_json_file_name_global, rx_json_file_name_global]
                copy_json_file()
                modify_json_file()
                if parameter_value != 'value':
                    value_list = [parameter_value, parameter_value]
                    print(value_list)
                    print(parameter_name)
                    deep_modify_json_file(per_case_output_path, json_file_list, json_file_mode, 'value',
                                     parameter_name, value_list)
                print('+++++++++++++++++++The "%s" is running++++++++++++++++ ' % case_name)
                if json_file_mode == "1_json_file":
                    txrx_process = Process(target=run_test, args=(json_file_list[0],'txrx'))
                    txrx_process.start()
                    txrx_process.join()
                else:
                    tx_process = Process(target=run_test, args=(json_file_list[0],'tx'))
                    rx_process = Process(target=run_test, args=(json_file_list[1],'rx'))
                    tx_process.start()
                    rx_process.start()
                    tx_process.join()
                    rx_process.join()

                print('+++++++++++++++++++++++++++++++The "%s" is finished++++++++++++++++++++++++' % case_name)
                check_test_result()












