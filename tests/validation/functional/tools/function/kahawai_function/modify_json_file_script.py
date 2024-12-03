import json
import os
import re
from collections import OrderedDict

special_case_list = ['interfaces_ip', 'interfaces_name', "sessions_ip"]
interfaces_list = ['interfaces_ip', 'interfaces_name']


def init_json_file(json_path, json_file_list, json_file_mode, port_dict, ip_dict, yuv):
    os.chdir(json_path)
    if json_file_mode == '1_json_file':
        with open(json_file_list[0], "r") as json_file_handle_r:
            json_content_dict = json.load(json_file_handle_r, object_pairs_hook=OrderedDict)
        json_content_dict['interfaces'][0]['name'] = port_dict['p_port']
        json_content_dict['interfaces'][0]['ip'] = ip_dict['tx_interfaces']
        json_content_dict['interfaces'][1]['name'] = port_dict['r_port']
        json_content_dict['interfaces'][1]['ip'] = ip_dict['rx_interfaces']
        json_content_dict['tx_sessions'][0]['dip'][0] = ip_dict['tx_sessions']
        json_content_dict['rx_sessions'][0]['ip'][0] = ip_dict['rx_sessions']
        json_content_dict['tx_sessions'][0]['video'][0]['video_url'] = yuv
        with open(json_file_list[0], "w") as json_file_handle_w:
            json.dump(json_content_dict, json_file_handle_w, indent=4)
    else:
        with open(json_file_list[0], "r") as tx_json_file_handle_r:
            tx_json_content_dict = json.load(tx_json_file_handle_r, object_pairs_hook=OrderedDict)
        tx_json_content_dict['interfaces'][0]['name'] = port_dict['p_port']
        tx_json_content_dict['interfaces'][0]['ip'] = ip_dict['tx_interfaces']
        tx_json_content_dict['tx_sessions'][0]['dip'][0] = ip_dict['tx_sessions']
        tx_json_content_dict['tx_sessions'][0]['video'][0]['video_url'] = yuv
        with open(json_file_list[0], "w") as tx_json_file_handle_w:
            json.dump(tx_json_content_dict, tx_json_file_handle_w, indent=4)

        with open(json_file_list[1], "r") as rx_json_file_handle_r:
            rx_json_content_dict = json.load(rx_json_file_handle_r, object_pairs_hook=OrderedDict)
        rx_json_content_dict['interfaces'][0]['name'] = port_dict['r_port']
        rx_json_content_dict['interfaces'][0]['ip'] = ip_dict['rx_interfaces']
        rx_json_content_dict['rx_sessions'][0]['ip'][0] = ip_dict['rx_sessions']
        with open(json_file_list[1], "w") as rx_json_file_handle_w:
            json.dump(rx_json_content_dict, rx_json_file_handle_w, indent=4)


def modify_json_file(json_path, json_file_list, json_file_mode, modify_mode, parameter_name, value_list):
    default_path = os.getcwd()
    os.chdir(json_path)
    line_index = 0
    print("call the deep modify json function")
    parameter_index_list = []
    if parameter_name in special_case_list:
        parameter = parameter_name.split("_")[1]
    else:
        parameter = '"' + parameter_name + '"'
    if json_file_mode == '1_json_file':
        json_file_name = json_file_list[0]
        with open(json_file_name, 'r') as json_file_handlie_r:
            json_content_list = json_file_handlie_r.readlines()
        for line in json_content_list:
            line_index = line_index + 1
            if parameter in line:
                parameter_index_list.append(line_index)
            if "tx_sessions" in line:
                tx_session_index = line_index
        if len(parameter_index_list) == 1:
            if "none" in value_list[0]:
                value_dict = {0: value_list[0], parameter_index_list[0]: value_list[1]}
            else:
                value_dict = {parameter_index_list[0]: value_list[0], 0: value_list[1]}
        elif len(parameter_index_list) == 2:
            value_dict = {parameter_index_list[0]: value_list[0], parameter_index_list[1]: value_list[1]}
        else:
            parameter_index_list_tmp = parameter_index_list[:]
            for parameter_index in parameter_index_list_tmp:
                if parameter_name in interfaces_list:
                    if parameter_index > tx_session_index:
                        parameter_index_list.remove(parameter_index)
                else:
                    if parameter_index < tx_session_index:
                        parameter_index_list.remove(parameter_index)
            value_dict = {parameter_index_list[0]: value_list[0], parameter_index_list[1]: value_list[1]}
        os.chdir(default_path)
        actually_modify_json_file(json_path, json_file_name, modify_mode, parameter, value_dict)
    else:
        for json_file_name in json_file_list:
            print(json_file_name)
            line_index = 0
            parameter_index_list = []
            if "tx.json" in json_file_name:
                sessions_tag = "tx_sessions"
            else:
                sessions_tag = "rx_sessions"
            with open(json_file_name, "r") as json_file_handle_r:
                json_content_list = json_file_handle_r.readlines()
            for line in json_content_list:
                line_index = line_index + 1
                if parameter in line:
                    parameter_index_list.append(line_index)
                if sessions_tag in line:
                    session_tag_index = line_index
            if len(parameter_index_list) > 1:
                parameter_index_list_tmp = parameter_index_list[:]
                for parameter_index in parameter_index_list_tmp:
                    if parameter_name in interfaces_list:
                        if parameter_index > session_tag_index:
                                parameter_index_list.remove(parameter_index)
                    else:
                        if parameter_index < session_tag_index:
                            parameter_index_list.remove(parameter_index)
            if len(parameter_index_list) == 0:
                continue
            else:
                if "tx.json" in json_file_name:
                    value_dict = {parameter_index_list[0]: value_list[0], 0: value_list[1]}
                    os.chdir(default_path)
                    actually_modify_json_file(json_path, json_file_name, modify_mode, parameter, value_dict)
                else:
                    value_dict = {0: value_list[0], parameter_index_list[0]: value_list[1]}
                    os.chdir(default_path)
                    actually_modify_json_file(json_path, json_file_name, modify_mode, parameter, value_dict)


def actually_modify_json_file(json_path, json_file_name, modify_mode, parameter, value_dict):
    default_path = os.getcwd()
    os.chdir(json_path)
    line_index = 0
    next_tag = 0
    tmp_json_file_name = os.path.splitext(json_file_name)[0] + "_tmp.json"
    with open(json_file_name, "r") as json_file_handle_r:
        json_content_list = json_file_handle_r.readlines()
    for line in json_content_list:
        line_index = line_index + 1
        if "parameter" in modify_mode:
            if parameter in line:
                print(line)
                print("=======================Above name of '%s' parameter was modified to ==============" % parameter)
                if line_index in value_dict.keys():
                    if '"' in parameter:
                        line = re.sub(parameter, '"' + value_dict[line_index] + '"', line)
                    else:
                        line = re.sub(parameter, value_dict[line_index], line)
                print(line)
        else:
            if line_index in value_dict.keys():
                if parameter in line or next_tag == 1:
                    print(line)
                    print("=================Above value of '%s' parameter was modified to ================" % parameter)
                    if next_tag == 0:
                        if isinstance(value_dict[line_index], int):
                            replace_value = str(value_dict[line_index])
                        else:
                            replace_value = '"' + value_dict[line_index] + '"'

                        if ': "' in line:
                            line = re.sub(':.*"', ': ' + replace_value, line)
                        elif ': [' in line:
                            next_tag = 1
                            line_index = line_index - 1
                        else:
                            line = re.sub(':.*[a-zA-Z0-9]', ': ' + replace_value, line)
                    else:
                        line = re.sub('.*.', replace_value, line)
                        next_tag = 0
                        line_index = line_index + 1
                    print(line)
        with open(tmp_json_file_name, "a") as json_file_handle_a:
            json_file_handle_a.write(line)
    os.remove(json_file_name)
    os.rename(tmp_json_file_name, json_file_name)
    with open(json_file_name, "r") as tmp_json_file_handle_r:
        json_content_dict = json.load(tmp_json_file_handle_r, object_pairs_hook=OrderedDict)
    with open(json_file_name, "w") as json_file_handle_w:
        json.dump(json_content_dict, json_file_handle_w, indent=4)
    os.chdir(default_path)


if __name__ == '__main__':
    port_dict_g = dict(p_port='0000:b1:00.0', r_port='0000:b1:00.1')
    ip_dict_g = dict(tx_interfaces='192.168.17.101',
                     rx_interfaces='92.168.17.102',
                     tx_sessions='239.168.48.9',
                     rx_sessions='239.168.48.9')
    json_file_list_g = (['wrong_valueson-video_format-rx_video_formatrx-tx_video_formatx-2_json_file_tx.json',
                         'wrong_valueson-video_format-rx_video_formatrx-tx_video_formatx-2_json_file_rx.json'])
    json_file_mode_g = '2_json_file'
    value_list_g = ["tx", "rx"]
    json_path_g = os.getcwd()
    init_json_file(json_path_g, json_file_list_g, json_file_mode_g, port_dict_g, ip_dict_g)
    modify_json_file(json_path_g, json_file_list_g, json_file_mode_g, "parameter", "replicas", value_list_g)
