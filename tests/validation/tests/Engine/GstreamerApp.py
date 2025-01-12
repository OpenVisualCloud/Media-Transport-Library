import hashlib
import os

from tests.Engine.execute import call, wait, log_fail, log_info

def create_connection_params(
    dev_port: str,
    payload_type: str,
    dev_ip: str = "192.168.96.3",
    ip: str = "192.168.96.2",
    udp_port: int = 20000,
    is_tx: bool = True
) -> dict:
    params = {
        "dev-port": dev_port,
    }
    if is_tx:
        params.update({
            "dev-ip": ip,
            "tx-ip": dev_ip,
            "tx-udp-port": udp_port,
            "tx-payload-type": payload_type,
        })
    else:
        params.update({
            "dev-ip": dev_ip,
            "rx-ip": ip,
            "rx-udp-port": udp_port,
            "rx-payload-type": payload_type
        })
    return params

def setup_gstreamer_tx_pipeline(
    build: str,
    nic_port_list: str,
    input_path: str,
    width: int,
    height: int,
    framerate: str,
    format: str,
    tx_payload_type: int,
    tx_queues: int,
    tx_framebuff_num: int = None,
    tx_fps: int = None
): 
    connection_params = create_connection_params(dev_port=nic_port_list, payload_type=tx_payload_type, is_tx=True)

    # st20 tx GStreamer command line
    pipeline_command = [
        "gst-launch-1.0", 
        "-v",
        "filesrc", 
        f"location={input_path}", 
        "blocksize=5529600",
        "!", 
        f"video/x-raw,format={format},height={height},width={width},framerate={framerate}/1",
        "!", 
        "mtl_st20p_tx",
        f"tx-queues={tx_queues}"
    ]

    if tx_framebuff_num is not None:
        pipeline_command.append(f"tx-framebuff-num={tx_framebuff_num}")

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    if tx_fps is not None:
        pipeline_command.append(f"tx-fps={tx_fps}")

    pipeline_command.append(f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/")

    return pipeline_command

def setup_gstreamer_rx_pipeline(
    build: str,
    nic_port_list: str,
    output_path: str,
    width: int,
    height: int,
    framerate: str,
    format: str,
    rx_payload_type: int,
    rx_queues: int,
    rx_framebuff_num: int = None,
    rx_fps: int = None
): 
    connection_params = create_connection_params(dev_port=nic_port_list, payload_type=rx_payload_type, is_tx=False)

    # st20 rx GStreamer command line
    pipeline_command = [
        "gst-launch-1.0", 
        "-v", 
        "mtl_st20p_rx",
        f"rx-queues={rx_queues}",
        f"rx-pixel-format={format}",
        f"rx-height={height}",
        f"rx-width={width}",
        f"rx-fps={framerate}"
    ]

    for key, value in connection_params.items():
        pipeline_command.append(f"{key}={value}")

    if rx_framebuff_num is not None:
        pipeline_command.append(f"rx-framebuff-num={rx_framebuff_num}")

    pipeline_command.extend(["!", "filesink", f"location={output_path}"])

    pipeline_command.append(f"--gst-plugin-path={build}/ecosystem/gstreamer_plugin/builddir/")

    return pipeline_command

def execute_test(
        build: str,
        tx_command: list,
        rx_command: list,
        fps: int,
        input_file: str,
        output_file: str,
        ):

    tx_process = call(' '.join(tx_command), cwd=build, timeout=120)
    rx_process = call(' '.join(rx_command), cwd=build, timeout=120)

    tx_output = wait(tx_process)
    rx_output = wait(rx_process)

    tx_result = check_tx_output(fps=fps, output=tx_output.splitlines())
    #rx_result = check_rx_output(fps=fps, output=rx_output.splitlines())

    file_compare = compare_files(input_file, output_file)

    if tx_result and file_compare:
        return True
    
    return False

def check_tx_output(fps: int, output: list) -> bool:
    tx_fps_result = None
    for line in output:
        if "TX_VIDEO_SESSION(0,0:st20sink): fps" in line:
            tx_fps_result = line
    if tx_fps_result is not None:
        for x in range(fps, fps-3, -1):
            if f"fps {x}" in tx_fps_result:
                log_info(f"FPS > {x}")
                return True

    log_fail("tx session failed")
    return False

def check_rx_output(fps: int, output: list) -> bool:
    rx_fps_result = None
    for line in output:
        if "RX_VIDEO_SESSION(0,0:st20src): fps" in line:
            rx_fps_result = line
    if rx_fps_result is not None:
        for x in range(fps, fps-2, -1):
            if f"fps {x}" in line:
                log_info(f"FPS > {x}")
                return True

    log_fail("rx session failed")
    return False

def compare_files(input_file, output_file):
    if os.path.exists(input_file) and os.path.exists(output_file):
        input_file_size = os.path.getsize(input_file)
        output_file_size = os.path.getsize(output_file)
        log_info(f"Input file size: {input_file_size}")
        log_info(f"Output file size: {output_file_size}")
        if input_file_size != output_file_size:
            return False

        with open(input_file, 'rb') as i_file, open(output_file, 'rb') as o_file:
            i_hash = hashlib.md5(i_file.read()).hexdigest()
            o_hash = hashlib.md5(o_file.read()).hexdigest()
            log_info(f"Input file hash: {i_hash}")
            log_info(f"Output file hash: {o_hash}")
            if i_hash == o_hash:
                return True

    log_fail("Comparison of files failed")
    return False

def format_change(file_format):
    if file_format in ["YUV422PLANAR10LE", "YUV_422_10bit"]:
        return "I422_10LE"
    else:
        return file_format