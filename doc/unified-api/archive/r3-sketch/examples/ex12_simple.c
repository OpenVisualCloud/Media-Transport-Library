/* 10 §14 — the L4 simple layer (mtl_simple.h): the ten-line loop P1 was promised. */
#include <mtl/experimental/mtl_simple.h>

#include "ex_common.h"

void render(void* addr, uint32_t stride);
void show(const void* addr, uint32_t stride);

int simple_tx(void) {
  mtl_simple_h tx;
  int ret = mtl_simple_tx_open("0000:af:01.0=192.168.1.10", "239.168.85.20:20000", 1920,
                               1080, "59.94", "yuv422p10le", &tx);
  if (ret < 0) return ex_fail("open", ret);
  while (g_running) {
    void* addr;
    uint32_t stride;
    ret = mtl_simple_tx_frame(tx, &addr, &stride, MTL_MS(100));
    if (ret == -MTL_ETIMEDOUT) continue;
    if (ret < 0) break;
    render(addr, stride);
    ret = mtl_simple_tx_send(tx);
    if (ret < 0) break;
  }
  if (ret < 0) ex_fail("tx", ret);
  return mtl_simple_close(tx);
}

int simple_rx(void) {
  mtl_simple_h rx;
  /* null backend: no NIC needed */
  int ret = mtl_simple_rx_open("null:1", "239.168.85.20:20000", 1920, 1080, "59.94",
                               "yuv422p10le", &rx);
  if (ret < 0) return ex_fail("open", ret);
  while (g_running) {
    const void* addr;
    uint32_t stride;
    ret = mtl_simple_rx_frame(rx, &addr, &stride, MTL_MS(100));
    if (ret == -MTL_ETIMEDOUT) continue;
    if (ret < 0) break;
    show(addr, stride);
    ret = mtl_simple_rx_done(rx);
    if (ret < 0) break;
  }
  if (ret < 0) ex_fail("rx", ret);
  return mtl_simple_close(rx);
}
