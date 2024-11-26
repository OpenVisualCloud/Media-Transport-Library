
def get_keyword(test_mode, parameter):
    if test_mode == "wrong_parameter_in_json":
        normal_keyword = "can not parse"
        normal_list = (['video_format', 'pacing', 'sessions_ip', 'pg_format',
                        'type', 'tr_offset', 'interfaces_name'])
        special_dict = dict(display='video_result', packing='video_result', payload_type='video_result',
                            start_port='invalid start port', interfaces_ip='invalid ip')

    elif test_mode == "wrong_value_in_json":
        normal_keyword = "video_result"
        normal_list = (['display', 'video_url'])
        special_dict = dict(packing='invalid video packing mode', start_port='invalid start port',
                            pacing='invalid video pacing', interfaces_name='get ip fail',
                            interfaces_ip='invalid ip', type='invalid video type', tr_offset='invalid video tr_offset',
                            sessions_ip='invalid ip', pg_format='invalid pixel group format',
                            payload_type='invalid payload', video_format='invalid video format',
                            video_url_nonexistent='open fail', video_url_txt_file='file size small then a frame')

    elif test_mode == "wrong_parameter_in_command":
        normal_keyword = "unrecognized option"
        normal_list = ['ebu', 'ptp', 'test_time']
        special_dict = dict(config_file='video_result')
    else:
        normal_keyword = "video_result"
        normal_list = ['test_time', 'config_file_txt']
        special_dict = dict(config_file='can not parse json file')

    if parameter in normal_list:
        result_keyword = normal_keyword
    else:
        result_keyword = special_dict[parameter]

    return result_keyword




