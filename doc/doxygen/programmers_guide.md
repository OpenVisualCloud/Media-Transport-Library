@page programmers_guide Programmer's Guide

@section software_interactions Software interactions

@subsection software_interactions_flow_tx_frame Transmitter frame mode

 lib takes resposibilty of rtp encapsulation 

 Below pseudo code presenting flows from application to the library for transmitter in frame mode.

 you could refer app/sample/tx_video_sample.c for the whole sample 

```bash
//create device
//set port interface and its ip. if two ports is use, seting num_port 2 and ST_PORT_R.
struct st_init_params param;
memset(&param, 0, sizeof(param));
param.num_ports = 1;
strncpy(param.port[ST_PORT_P], "xxxx:xx:xx.x", ST_PORT_MAX_LEN);
uint8_t sip[4]={xx,xx,xx,xx};
memcpy(param.sip_addr[ST_PORT_P], sip, ST_IP_ADDR_LEN);
param.flags = ST_FLAG_BIND_NUMA; // default bind to numa
param.log_level = ST_LOG_LEVEL_ERROR; // log level. ERROR, INFO, WARNING
param.priv = ctx; // usr ctx pointer
param.ptp_get_time_fn = test_ptp_from_real_time; // user regist ptp func, if not regist, the internal ptp will be used
param.tx_sessions_cnt_max = session_num; // max tx session set by user
param.rx_sessions_cnt_max = 0; // max rx session set by user
dev_handle = st_init(&param);

//create and register tx session
for (i = 0; i < session_num; i++)
{
    struct st20_tx_ops ops_tx;
    ops_tx.name = "st20_test";
    ops_tx.priv = app; // app handle register to lib
    ops_tx.num_port = 1;
    uint8_t ip[4]={xx,xx,xx,xx};
    memcpy(ops_tx.dip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN); //tx dst ip like 239.0.0.1
    strncpy(ops_tx.port[ST_PORT_P], "xxxx:xx:xx.x", ST_PORT_MAX_LEN); // send port interface like 0000:af:00.0
    ops_tx.udp_port[ST_PORT_P] = 10000 + i; // user could config the udp port in this interface.
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_FRAME_LEVEL;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.framebuff_cnt = 3; //frame buffer pool number in lib
    ops_tx.get_next_frame = get_next_frame_from_app; //app regist callback(none-blocked func), lib call this API
    to get the frame buffer needed to send from app, if frame is not ready, return -1.
    ops_tx.notify_frame_done = notify_frame_done; //app regist non-block func, app could get the frame tx done
    notification info by this callback
    ops_tx->payload_type = 112; // you could set the payload type by your self. if not set, the default will be used
    tx_handle[i] = st20_tx_create(dev_handle, &ops_tx);
}
//start tx
ret = st_start(dev_handle);

xxxx

//stop tx
ret = st_stop(dev_handle);

//release session
for (i = 0; i < session_num; i++)
{
   ret = st20_tx_free(tx_handle[i]);
}

//destroy device
st_uninit(dev_handle);
```
app thread to get the frame buf and feed data to it, you could refer tx_video_app.c in app/src folder

```bash
static void* app_tx_video_frame_thread(void* arg) {
  //wait the frame tx done notification from notify_frame_done

  xxxx
  xxxxx

  uint8_t* dst = st20_tx_get_framebuffer(session_handle, free_buf_idx); // session handle is the tx_handle created by st20_tx_create
  memcpy(dst, frame_buf_data, frame_buf_len);
  //put the free buf idx to ready buf idx queue. and when get_next_frame_from_app is called, return the ready buf idx to lib. then lib could get the feeded frame.

  xxxx
  xxxxx
}
```
\n

@subsection software_interactions_flow_tx_rtp Transmitter rtp mode
 
 app takes resposibilty of rtp encapsulation

 Below pseudo code presenting flows from application to the library for transmitter in rtp mode.

 take st20 as example, if st30/st40/st22 session is created/release, replace st20_xxx with st30_xxx, st40_xxx, st22_xxx

 you could refer app/sample/tx_rtp_video_sample.c for the whole sample of st22 case

```bash
//create device
//set port interface and its ip. if two ports is use, seting num_port 2 and ST_PORT_R.
struct st_init_params param;
memset(&param, 0, sizeof(param));
param.num_ports = 1;
strncpy(param.port[ST_PORT_P], "xxxx:xx:xx.x", ST_PORT_MAX_LEN);
uint8_t sip[4]={xx,xx,xx,xx};
memcpy(param.sip_addr[ST_PORT_P], sip, ST_IP_ADDR_LEN);
param.flags = ST_FLAG_BIND_NUMA; // default bind to numa
param.log_level = ST_LOG_LEVEL_ERROR; // log level. ERROR, INFO, WARNING
param.priv = ctx; // usr ctx pointer
param.ptp_get_time_fn = test_ptp_from_real_time; // user regist ptp func, if not regist, the internal ptp will be used
param.tx_sessions_cnt_max = session_num; // max tx session set by user
param.rx_sessions_cnt_max = 0; // max rx session set by user
dev_handle = st_init(&param);

//create and register tx session
for (i = 0; i < session_num; i++)
{
    struct st20_tx_ops ops_tx;
    ops_tx.name = "st20_test";
    ops_tx.priv = app; // app handle register to lib
    ops_tx.num_port = 1;
    uint8_t ip[4]={xx,xx,xx,xx};
    memcpy(ops_tx.dip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN); //tx dst ip like 239.0.0.1
    strncpy(ops_tx.port[ST_PORT_P], "xxxx:xx:xx.x", ST_PORT_MAX_LEN); // send port interface like 0000:af:00.0
    ops_tx.udp_port[ST_PORT_P] = 10000 + i; // user could config the udp port in this interface.
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.notify_rtp_done = notify_rtp_done; //app regist non-block func, app could get the rtp tx done notification info by this callback
    ops_tx->rtp_ring_size = 1024; // the length of rte ring shared between the app and lib. app put the rtp packet to the ring and lib get it out and send. the length should be 2^n
    ops_tx->rtp_frame_total_pkts = total_pkt_in_pkts; // rtp frame total pkt and rtp pkt size will be used in ratelimiting to guarantee the pacing.
    ops_tx->rtp_pkt_size = rtp_pkt_size;
    ops_tx->payload_type = 112; // you could set the payload type by your self. if not set, the default will be used
    tx_handle[i] = st20_tx_create(dev_handle, &ops_tx);
}
//start tx
ret = st_start(dev_handle);

xxxxx

//stop tx
ret = st_stop(dev_handle);

//release session
for (i = 0; i < session_num; i++)
{
   ret = st20_tx_free(tx_handle[i]);
}

//destroy device
st_uninit(dev_handle);
```

app thread to feed encapsulated rtp buf, you could refer tx_video_app.c in app/src folder
```bash
static void* app_tx_video_rtp_thread(void* arg) {
  //wait the rtp tx done notification from notify_rtp_done if there is no available mbuf in the rte ring.

  xxxx
  xxxxx

  void** mbuf = st20_tx_get_mbuf(session_handle, usrptr); // session handle is the tx_handle created by st20_tx_create
  //feed the rtp payload and rtp headr into usrptr, return the mbuf to lib by st20_tx_put_mbuf, mbuf_len is the feed rtp buf len.
  xxxx
  st20_tx_put_mbuf(session_handle, mbuf, mbuf_len);
  xxxx
  xxxxx
}
```


\n

@subsection software_interactions_flow_rx_frame Receiver frame mode

 lib takes resposibilty of rtp decapsulation

 Below pseudo code presenting flows from application to the library for receiver in frame mode.

 you could refer app/sample/rx_video_sample.c for the whole sample 

```bash
//create device
//set port interface and its ip. if two ports is use, seting num_port 2 and ST_PORT_R.
struct st_init_params param;
memset(&param, 0, sizeof(param));
param.num_ports = 1;
strncpy(param.port[ST_PORT_P], "xxxx:xx:xx.x", ST_PORT_MAX_LEN);
uint8_t sip[4]={xx,xx,xx,xx};
memcpy(param.sip_addr[ST_PORT_P], sip, ST_IP_ADDR_LEN);
param.flags = ST_FLAG_BIND_NUMA; // default bind to numa
param.log_level = ST_LOG_LEVEL_ERROR; // log level. ERROR, INFO, WARNING
param.priv = ctx; // usr ctx pointer
param.ptp_get_time_fn = test_ptp_from_real_time; // user regist ptp func, if not regist, the internal ptp will be used
param.tx_sessions_cnt_max = 0; // max tx session set by user
param.rx_sessions_cnt_max = session_num; // max rx session set by user
dev_handle = st_init(&param);

//create and register rx session
for (i = 0; i < session_num; i++)
{
    struct st20_rx_ops ops_rx;
    ops_rx.name = "st20_test";
    ops_rx.priv = app; // app handle register to lib
    ops_rx.num_port = 1;
    uint8_t ip[4]={xx,xx,xx,xx};
    memcpy(ops_rx.sip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN); //tx src ip like 239.0.0.1
    strncpy(ops_rx.port[ST_PORT_P], "xxxx:xx:xx.x", ST_PORT_MAX_LEN); // send port interface like 0000:af:00.0
    ops_rx.udp_port[ST_PORT_P] = 10000 + i; // user could config the udp port in this interface.
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.framebuff_cnt = 3;
    ops_rx.notify_frame_ready = rx_frame_ready; //app regist non-block func, app could get a frame ready notification info by this callback
    rx_handle[i] = st20_rx_create(dev_handle, &ops_rx);
}
//start rx
ret = st_start(dev_handle);

xxxxx

//stop rx
ret = st_stop(dev_handle);

//release session
for (i = 0; i < session_num; i++)
{
   ret = st20_rx_free(rx_handle[i]);
}

//destroy device
st_uninit(dev_handle);
```
app thread to get the frame buf, dispose it and return to lib.you could refer rx_video_app.c in app/src folder

```bash
static void* app_rx_video_frame_thread(void* arg) {
  //wait the frame rx done notification from rx_frame_ready

  xxxx
  xxxxx
  // get the RX frame and deal with it. the ready frame idx is got in rx_frame_ready

  // return the disposed frame to lib
  st20_rx_put_framebuff(session_handle, frame); // session handle is the rx_handle created by st20_rx_create

  xxxx
  xxxxx
}
```

\n

@subsection software_interactions_flow_rx_rtp Receiver rtp mode

 app takes resposibilty of rtp decapsulation

 Below pseudo code presenting flows from application to the library for receiver in rtp mode.

you could refer app/sample/rx_rtp_video_sample.c for the whole sample of st22 case

```bash
//create device
//set port interface and its ip. if two ports is use, seting num_port 2 and ST_PORT_R.
struct st_init_params param;
memset(&param, 0, sizeof(param));
param.num_ports = 1;
strncpy(param.port[ST_PORT_P], "xxxx:xx:xx.x", ST_PORT_MAX_LEN);
uint8_t sip[4] = {xx,xx,xx,xx};
memcpy(param.sip_addr[ST_PORT_P], sip, ST_IP_ADDR_LEN);
param.flags = ST_FLAG_BIND_NUMA; // default bind to numa
param.log_level = ST_LOG_LEVEL_ERROR; // log level. ERROR, INFO, WARNING
param.priv = ctx; // usr ctx pointer
param.ptp_get_time_fn = test_ptp_from_real_time; // user regist ptp func, if not regist, the internal ptp will be used
param.tx_sessions_cnt_max = 0; // max tx session set by user
param.rx_sessions_cnt_max = session_num; // max rx session set by user
dev_handle = st_init(&param);

//create and register rx session
for (i = 0; i < session_num; i++)
{
    struct st20_rx_ops ops_rx;
    ops_rx.name = "st20_test";
    ops_rx.priv = app; // app handle register to lib
    ops_rx.num_port = 1;
    uint8_t ip[4] = {xx,xx,xx,xx};
    memcpy(ops_rx.sip_addr[ST_PORT_P], ip, ST_IP_ADDR_LEN); //tx src ip like 239.0.0.1
    strncpy(ops_rx.port[ST_PORT_P], "xxxx:xx:xx.x", ST_PORT_MAX_LEN); // send port interface like 0000:af:00.0
    ops_rx.udp_port[ST_PORT_P] = 10000 + i; // user could config the udp port in this interface.
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_RTP_LEVEL;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.notify_rtp_ready = rx_rtp_ready; //app regist non-block func, app could get a rtp ready
    notification info by this callback
    ops_rx.rtp_ring_size = 1024; // the length of rte ring shared between the app and lib. lib put the rx rtp packet to the ring and app gets it out to consume. the length should be 2^n
    rx_handle[i] = st20_rx_create(dev_handle, &ops_rx);
}
//start rx
ret = st_start(dev_handle);

//stop rx
ret = st_stop(dev_handle);

//release session
for (i = 0; i < session_num; i++)
{
   ret = st20_rx_free(rx_handle[i]);
}

//destroy device
st_uninit(dev_handle);
```

app thread to consume rtp, you could refer rx_video_app.c in app/src folder

```bash
static void* app_rx_video_rtp_thread(void* arg) {
  //wait the rtp rx done notification from rx_rtp_ready if there is no mbuf in the rte_ring

  xxxx
  xxxxx
  // get the RX rtp and deal with it.
  mbuf = st20_rx_get_mbuf(session_handle, &usrptr, &len); // session handle is the rx_handle created by st20_rx_create
  // deal with the usrptr, it contains the rtp header and payload
  // free to lib
  st20_rx_put_mbuf(session_handle, mbuf);
  xxxx
  xxxxx
}
```
\n
