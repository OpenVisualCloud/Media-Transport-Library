/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

#include <Windows.h>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include <mtl/mtl_api.h>
#include <mtl/st20_api.h>
#include <mtl/st_pipeline_api.h>

static std::mutex mtx;
static std::condition_variable cv;
static uint32_t fb_done = 0;
static bool stop = false;
static mtl_handle g_st = NULL;

void signalHandler(int signum) {
  std::cout << "SIGINT received - exiting!\n";
  switch (signum) {
    case SIGINT:
      stop = true;
      if (g_st != NULL) mtl_abort(g_st);
      break;
  }
}

int main() {
  int ret = 0;
  std::cout << "Starting IMTL sample..." << std::endl << mtl_version() << std::endl;

  std::signal(SIGINT, signalHandler);

  /* setting of parameters */
  struct mtl_init_params param;
  memset(&param, 0, sizeof(param));
  strncpy_s(param.port[MTL_PORT_P], "0000:03:00.0", MTL_PORT_MAX_LEN);
  param.num_ports = 1;
  param.sip_addr[MTL_PORT_P][0] = 192;
  param.sip_addr[MTL_PORT_P][1] = 168;
  param.sip_addr[MTL_PORT_P][2] = 96;
  param.sip_addr[MTL_PORT_P][3] = 12;
  param.log_level = MTL_LOG_LEVEL_INFO;
  param.tx_sessions_cnt_max = 1;
  param.rx_sessions_cnt_max = 0;

  /* init mtl */
  mtl_handle st = mtl_init(&param);
  if (st == NULL) {
    std::cout << "mtl_init fail" << std::endl;
    return -EIO;
  }
  g_st = st;

  /* create a st2110-20 pipeline tx session */
  struct st20p_tx_ops ops_tx;
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = "st20p_tx_sample";
  ops_tx.port.num_port = 1;
  uint8_t dip[4] = {239, 19, 96, 1};
  memcpy(ops_tx.port.dip_addr[MTL_SESSION_PORT_P], dip, MTL_IP_ADDR_LEN);
  strncpy_s(ops_tx.port.port[MTL_SESSION_PORT_P], param.port[MTL_PORT_P],
            MTL_PORT_MAX_LEN);
  ops_tx.port.udp_port[MTL_SESSION_PORT_P] = 20000;
  ops_tx.port.payload_type = 112;
  ops_tx.width = 1920;
  ops_tx.height = 1080;
  ops_tx.fps = ST_FPS_P59_94;
  ops_tx.input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  ops_tx.transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_tx.framebuff_cnt = 3;
  auto sample_frame_available = [](void* priv) {
    cv.notify_one();
    return 0;
  };
  auto sample_frame_done = [](void* priv, struct st_frame* frame) {
    fb_done++;
    return 0;
  };
  ops_tx.notify_frame_available = sample_frame_available;
  ops_tx.notify_frame_done = sample_frame_done;

  st20p_tx_handle tx_handle = st20p_tx_create(st, &ops_tx);
  if (tx_handle == NULL) {
    mtl_uninit(st);
    return -EIO;
  }

  auto sample_frame_thread = [tx_handle]() {
    struct st_frame* frame;
    while (!stop) {
      frame = st20p_tx_get_frame(tx_handle);
      if (!frame) { /* no frame */
        {
          std::unique_lock<std::mutex> lock(mtx);
          cv.wait(lock);
        }
        continue;
      }

      // do something to frame

      st20p_tx_put_frame(tx_handle, frame);
    }
  };

  std::thread frame_thread(sample_frame_thread);

  ret = mtl_start(st);

  while (!stop) {
    Sleep(1000);
  }

  cv.notify_one();
  frame_thread.join();
  if (tx_handle) st20p_tx_free(tx_handle);

  ret = mtl_stop(st);

  /* uninit mtl */
  if (st != NULL) {
    mtl_uninit(st);
    st = NULL;
  }

  std::cout << "Stopped, fb_done: " << fb_done << std::endl;

  return ret;
}
