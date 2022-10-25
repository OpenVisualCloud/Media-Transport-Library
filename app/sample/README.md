# Sample code for how to develop application.

## 1. Introduction:
The dir incldue the simple sample code for how to develop application quickly based on Kahawai library.

## 2. Pipeline samples which based on get/put APIs:
[tx_st20_pipeline_sample.c](tx_st20_pipeline_sample.c): A tx video(st2110-20) application based on pipeline interface, library will call the color format conversion in case the input format is not the one defined in RFC4175. Application can focus on the frame buffer producer.
```bash
ST_PORT_P=0000:af:00.1 ./build/app/TxSt20PipelineSample
```

[rx_st20_pipeline_sample.c](rx_st20_pipeline_sample.c): A rx video(st2110-20) application based on pipeline interface, library will call the color format conversion in case the output format is not the one defined in RFC4175. Application can focus on the frame buffer consumer.
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxSt20PipelineSample
```

[tx_st22_pipeline_sample.c](tx_st22_pipeline_sample.c): A tx compressed video(st2110-22) application based on pipeline interface, library will call the registered encoder plugins. Application only need focus on the frame buffer producer.
```bash
ST_PORT_P=0000:af:00.1 ./build/app/TxSt22PipelineSample
```

[rx_st22_pipeline_sample.c](rx_st22_pipeline_sample.c): A rx compressed video(st2110-22) application based on pipeline interface, library will call the registered decoder plugins. Application only need focus on the frame buffer consumer.
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxSt22PipelineSample
```

[rx_st20p_tx_st22_fwd.c](rx_st20p_tx_st22_fwd.c): A demo application which receive a st20 stream and output as st22 compressed stream with logo rendering.
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxSt20pTxSt22pFwd
```

[rx_st20p_tx_st20_fwd.c](rx_st20p_tx_st20_fwd.c): A demo application which receive a st20 stream and output as st20 stream with logo rendering.
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxSt20pTxSt20pFwd
```

## 3. Samples which based on low level APIs for advanced usage:
[tx_video_sample.c](tx_video_sample.c): A tx video(st2110-20) application based on frame interface, application need charge the color format conversion in case the user format is not the one defined in RFC4175.
```bash
ST_PORT_P=0000:af:00.1 ./build/app/TxVideoSample
```

[rx_video_sample.c](rx_video_sample.c): A rx video(st2110-20) application based on frame interface, application need charge the color format conversion in case the user format is not the one defined in RFC4175.
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxVideoSample
```

[tx_slice_video_sample.c](tx_slice_video_sample.c): A tx video(st2110-20) application based on slice interface for low latency requirement, application need handle the color format conversion in case the user format is not the one defined in RFC4175.
```bash
ST_PORT_P=0000:af:00.1 ./build/app/TxSliceVideoSample
```

[rx_slice_video_sample.c](rx_slice_video_sample.c): A rx video(st2110-20) application based on slice interface for low latency requirement, application need handle the color format conversion in case the user format is not the one defined in RFC4175.
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxSliceVideoSample
```

[tx_rtp_video_sample.c](tx_rtp_video_sample.c): A tx video(st2110-20) application based on rtp interface, application need handle all rtp pack.
```bash
ST_PORT_P=0000:af:00.1 ./build/app/TxRtpVideoSample
```

[rx_rtp_video_sample.c](rx_rtp_video_sample.c): A rx video(st2110-20) application based on rtp interface application need handle all rtp unpack.
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxRtpVideoSample
```

[tx_st22_video_sample.c](tx_st22_video_sample.c): A tx video(st2110-22) application based on frame interface, application need handle the encoder.
```bash
ST_PORT_P=0000:af:00.1 ./build/app/TxSt22VideoSample
```

[rx_st22_video_sample.c](rx_st22_video_sample.c): A rx video(st2110-22) application based on frame interface, application need handle the decoder.
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxSt22VideoSample
```

[tx_video_split_sample.c](tx_video_split_sample.c): A tx video(st2110-20) application based on frame interface, application reads a series of 4k frames from the file, square splits them to 4 parts and sends with 4 1080p sessions.
```bash
ST_PORT_P=0000:af:00.1 YUVFILE=./test.yuv ./build/app/TxVideoSplitSample
```

[rx_st20_redundant_sample.c](rx_st20_redundant_sample.c): A rx video(st2110-20) redundant application based on frame interface, application need handle the color format conversion in case the user format is not the one defined in RFC4175.
```bash
ST_PORT_P=0000:af:01.0 ST_PORT_R=0000:af:01.1 ./build/app/RxSt20RedundantSample
```

## 4. Dma samle:
[dma_sample.c](dma_sample.c): A DMA sample code.