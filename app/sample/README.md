# Sample code for how to develop application.

## 1. Introduction:
The dir incldue the simple sample code for how to develop application quickly based on Kahawai library.

## 2. Pipeline samples which based on get/put APIs:
[tx_st20_pipeline_sample.c](tx_st20_pipeline_sample.c): A tx video(st2110-20) application based on pipeline interface, library will call the color format conversion in case the input format is not the one defined in RFC4175. Application can focus on the frame buffer producer.
```bash
./build/app/TxSt20PipelineSample --p_port 0000:af:01.0 --p_sip 192.168.75.11 --p_tx_ip 239.168.75.20
```

[rx_st20_pipeline_sample.c](rx_st20_pipeline_sample.c): A rx video(st2110-20) application based on pipeline interface, library will call the color format conversion in case the output format is not the one defined in RFC4175. Application can focus on the frame buffer consumer.
```bash
./build/app/RxSt20PipelineSample --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20
```

[tx_st22_pipeline_sample.c](tx_st22_pipeline_sample.c): A tx compressed video(st2110-22) application based on pipeline interface, library will call the registered encoder plugins. Application only need focus on the frame buffer producer.
```bash
./build/app/TxSt22PipelineSample --p_port 0000:af:01.0 --p_sip 192.168.75.11 --p_tx_ip 239.168.75.20
```

[rx_st22_pipeline_sample.c](rx_st22_pipeline_sample.c): A rx compressed video(st2110-22) application based on pipeline interface, library will call the registered decoder plugins. Application only need focus on the frame buffer consumer.
```bash
./build/app/RxSt22PipelineSample --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20
```

[rx_st20p_tx_st22p_fwd.c](rx_st20p_tx_st22p_fwd.c): A demo application which receive a st20 stream and output as st22 compressed stream with logo rendering.
```bash
./build/app/RxSt20pTxSt22pFwd --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20 --p_fwd_ip 239.168.75.21
```

[rx_st20p_tx_st20p_fwd.c](rx_st20p_tx_st20p_fwd.c): A demo application which receive a st20 stream and output as st20 stream with logo rendering.
```bash
./build/app/RxSt20pTxSt20pFwd --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20 --p_fwd_ip 239.168.75.21
```

[rx_st20p_tx_st20p_downsample_fwd.c](rx_st20p_tx_st20p_fwd.c): A demo application which receive a 4k st20 stream and output as st20 stream downscaled to 1080p.
```bash
./build/app/RxSt20pTxSt20pDownsampleFwd --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20 --p_fwd_ip 239.168.75.21 --width 3840 --height 2160
```

[rx_st20p_tx_st20p_split_fwd.c](rx_st20p_tx_st20p_split_fwd.c): Receive 4k frames from rx, do square quad division and send with 4 1080p sessions.
```bash
./build/app/RxSt20pTxSt20pSplitFwd --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20 --p_fwd_ip 239.168.75.20 --width 3840 --height 2160
```

[rx_st20p_tx_st20p_merge_fwd.c](rx_st20p_tx_st20p_merge_fwd.c): Receive 4 1080p sessions from rx, merge to single 4k st20 stream and send out.
```bash
./build/app/RxSt20pTxSt20pMergeFwd --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20 --p_fwd_ip 239.168.75.20 --width 3840 --height 2160
```

[rx_st20p_tx_st20p_downsample_merge_fwd.c](rx_st20p_tx_st20p_merge_fwd.c): Receive 4 1080p sessions from rx, downsample and merge to single 1080p st20 stream and send out.
```bash
./build/app/RxSt20pTxSt20pMergeFwd --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20 --p_fwd_ip 239.168.75.20
```

## 3. Samples which based on low level APIs for advanced usage:
[tx_video_sample.c](tx_video_sample.c): A tx video(st2110-20) application based on frame interface, application need charge the color format conversion in case the user format is not the one defined in RFC4175.
```bash
./build/app/TxVideoSample --p_port 0000:af:01.0 --p_sip 192.168.75.11 --p_tx_ip 239.168.75.20
```

[rx_video_sample.c](rx_video_sample.c): A rx video(st2110-20) application based on frame interface, application need charge the color format conversion in case the user format is not the one defined in RFC4175.
```bash
./build/app/RxVideoSample --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20
```

[tx_slice_video_sample.c](tx_slice_video_sample.c): A tx video(st2110-20) application based on slice interface for low latency requirement, application need handle the color format conversion in case the user format is not the one defined in RFC4175.
```bash
./build/app/TxSliceVideoSample --p_port 0000:af:01.0 --p_sip 192.168.75.11 --p_tx_ip 239.168.75.20
```

[rx_slice_video_sample.c](rx_slice_video_sample.c): A rx video(st2110-20) application based on slice interface for low latency requirement, application need handle the color format conversion in case the user format is not the one defined in RFC4175.
```bash
./build/app/RxSliceVideoSample --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20
```

[tx_rtp_video_sample.c](tx_rtp_video_sample.c): A tx video(st2110-20) application based on rtp interface, application need handle all rtp pack.
```bash
./build/app/TxRtpVideoSample --p_port 0000:af:01.0 --p_sip 192.168.75.11 --p_tx_ip 239.168.75.20
```

[rx_rtp_video_sample.c](rx_rtp_video_sample.c): A rx video(st2110-20) application based on rtp interface application need handle all rtp unpack.
```bash
./build/app/RxRtpVideoSample --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20
```

[tx_st22_video_sample.c](tx_st22_video_sample.c): A tx video(st2110-22) application based on frame interface, application need handle the encoder.
```bash
./build/app/TxSt22VideoSample --p_port 0000:af:01.0 --p_sip 192.168.75.11 --p_tx_ip 239.168.75.20
```

[rx_st22_video_sample.c](rx_st22_video_sample.c): A rx video(st2110-22) application based on frame interface, application need handle the decoder.
```bash
./build/app/RxSt22VideoSample --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20
```

[tx_video_split_sample.c](tx_video_split_sample.c): A tx video(st2110-20) application based on frame interface, application reads a series of 4k frames from the file, square splits them to 4 parts and sends with 4 1080p sessions.
```bash
./build/app/TxVideoSplitSample --p_port 0000:af:01.0 --p_sip 192.168.75.11 --p_tx_ip 239.168.75.20
```

[rx_st20_tx_st20_split_fwd.c](rx_st20_tx_st20_split_fwd.c): Receive 4k frames from rx, do square quad division and send with 4 1080p sessions.
```bash
./build/app/RxSt20TxSt20SplitFwd --p_port 0000:af:01.1 --p_sip 192.168.75.22 --p_rx_ip 239.168.75.20 --p_fwd_ip 239.168.75.20 --width 3840 --height 2160
```

[rx_st20_redundant_sample.c](rx_st20_redundant_sample.c): A rx video(st2110-20) redundant application based on frame interface, application need handle the color format conversion in case the user format is not the one defined in RFC4175.
```bash
./build/app/RxSt20RedundantSample --p_port 0000:af:01.0 --r_port 0000:af:01.1 --p_sip 192.168.77.11 --r_sip 192.168.77.12 --p_rx_ip 239.168.77.20 --r_rx_ip 239.168.77.21
```

## 4. DMA sample:
[dma_sample.c](dma_sample.c): A DMA sample code.