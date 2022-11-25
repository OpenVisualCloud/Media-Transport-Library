# External Frame API Guide

## 1. Overview

In the general API, the video frames used in MTL are allcated by library with rte_malloc(usually with hugepages).
In some use cases, each frame data needs to be copied from/to user. When it's UHD or high framerate video, the copying can cause CPU/memory stall and become bottleneck of the whole pipeline.
The external frame API is introduced so the library can use user provided memory to receive/send st2110-20 frames, or as the color format conversion destination/source in the pipeline API.

## 2. ST20 Pipeline ext_frame API

### 2.1 st_ext_frame

```c
/** The structure info for external frame */
struct st_ext_frame {
  /** Each plane's virtual address of external frame */
  void* addr[ST_MAX_PLANES];
  /** Each plane's IOVA of external frame */
  mtl_iova_t iova[ST_MAX_PLANES];
  /** Each plane's linesize of external frame, leave as 0 if no line padding */
  size_t linesize[ST_MAX_PLANES];
  /** Buffer size of external frame */
  size_t size;
  /** Private data for user */
  void* opaque;
};
```

### 2.2 st20p_tx usages

#### 2.2.1 dynamic frames

in ops, set the flag

```c
ops_tx.flags |= ST20P_TX_FLAG_EXT_FRAME;
```

when sending a frame, get the frame and put with ext_frame info

```c
frame = st20p_tx_get_frame(handle);
struct st_ext_frame ext_frame;
uint8_t planes = st_frame_fmt_planes(frame->fmt);
for(int i = 0; i < planes; i++) {
    ext_frame.addr[i] = your_addr[i];
    ext_frame.iova[i] = your_iova[i]; // must provide IOVA for no convert mode
    ext_frame.linesize[i] = your_linesize[i];
}
ext_frame.size = your_frame_size;
ext_frame.opaque = your_frame_handle;
st20p_tx_put_ext_frame(handle, frame, &ext_frame);
```

when the library finished handling the frame, it will notify by callback, you can return the frame buffer here

```c
// set the callback in ops
ops_tx.notify_frame_done = tx_st20p_frame_done;
// ...
// implement the callback
static int tx_st20p_frame_done(void* priv, struct st_frame*frame) {
    ctx* s = priv;
    your_frame_handle = frame->opaque;
    your_frame_free(your_frame_handle);
    return 0;
}
```

### 2.3 st20p_rx usages

#### 2.3.1 dedicated frames

set the ext_frames array in ops

```c
struct st_ext_frame ext_frames[fb_cnt];
for (int i = 0; i < fb_cnt; ++i) {
    uint8_t planes = st_frame_fmt_planes(frame->fmt);
    for(int plane = 0; plane < planes; plane++) {
        ext_frames[i].addr[plane] = your_addr[plane];
        ext_frames[i].iova[plane] = your_iova[plane]; // must provide IOVA for no convert mode
        ext_frames[i].linesize[plane] = your_linesize[plane];
    }
    ext_frames[i].size = your_frame_size;
    ext_frames[i].opaque = your_frame_handle;
}
ops_rx.ext_frames = ext_frames;
rx_handle = st20p_rx_create(st, &ops_rx);
```

use as the general API

#### 2.3.2 dynamic frames usage - convert mode

in ops, set the flag

```c
ops_rx.flags |= ST20P_RX_FLAG_EXT_FRAME;
```

when receiving a frame, get the frame with ext_frame info and put the frame

user should maintain the lifetime of frames

#### 2.3.3 dynamic frames usage - no convert mode

(the same usage as raw video api 3.3.2)  
implement and set query_ext_frame callback and set incomplete frame flag

```c
// set the callback in ops
// set the incomplete frame flag
ops_rx.query_ext_frame = rx_query_ext_frame;
ops_rx.flags |= ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
//...
//implement the callback
static int rx_query_ext_frame(void* priv, st20_ext_frame*ext_frame, struct st20_rx_frame_meta* meta) {
    ctx* s = (ctx*)priv;
    ext_frame->buf_addr = your_addr[0];
    ext_frame->buf_iova = your_iova[0];
    ext_frame->buf_len = your_frame_size;
    ext_frame->opaque = your_frame_handle;
    return 0;
}
```

use as the general API, user should maintain the lifetime of frames

## 3. ST20(raw video) ext_frame API

### 3.1 st20_ext_frame

```c
/** External framebuffer */
struct st20_ext_frame {
  /** Virtual address of external framebuffer */
  void* buf_addr;
  /** DMA mapped IOVA of external framebuffer */
  mtl_iova_t buf_iova;
  /** Length of external framebuffer */
  size_t buf_len;
  /** Private data for user, will be retrived with st_frame or st20_rx_frame_meta */
  void* opaque;
};
```

### 3.2 st20_tx usages

#### 3.2.1 dynamic frames usage

in ops, set the flag

```c
ops_rx.flags |=ST20_TX_FLAG_EXT_FRAME;
```

explcitly set the ext frame, and in query_next_frame callback, provide the index

```c
st20_tx_set_ext_fram(s->handle, idx, &ext_frame);
// in query_next_frame
*next_frame_idx = idx;
```

### 3.3 st20_rx usages

#### 3.3.1 dedicated frames usage

set the ext_frames array in ops

```c
struct st20_ext_frameext_frames[fb_cnt];
for (int i = 0; i < fb_cnt;++i) {
    ext_frames[i].buf_addr = your_addr;
    ext_frames[i].buf_iova = your_iova;
    ext_frames[i].buf_len = your_frame_size;
    ext_frames[i].opaque = your_frame_handle;
}
ops_rx.ext_frames =ext_frames;
rx_handle = st20_rx_creat(st, &ops_rx);
```

use as the general API

#### 3.3.2 dynamic frames usage

implement and set query_ext_frame callback and set incomplete frame flag

```c
// set the callback in ops
// set the incomplete frameflag
ops_rx.query_ext_frame =rx_query_ext_frame;
ops_rx.flags |=ST20_RX_FLAG_RECEIVE_INCOMPLEE_FRAME;
//...
//implement the callback
static int rx_query_ext_fram(void* priv, st20_ext_frame*ext_frame, structst20_rx_frame_meta* meta) {
    ctx* s = (ctx*)priv;
    ext_frame->buf_addr = your_addr;
    ext_frame->buf_iova = your_iova;
    ext_frame->buf_len = your_frame_size;
    ext_frame->opaque = your_frame_handle;
    return 0;
}
```

use as the general API, user should maintain the lifetime of frames
