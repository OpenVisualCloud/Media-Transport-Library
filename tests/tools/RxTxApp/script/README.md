# test script helper

## 1. Prepare the test resource files, copy to current folder

* test.yuv for 1080p video
* test_720p.yuv for 720p video
* test_4k.yuv for 4k video
* test_8k.yuv for 4k video
* test.pcm for audio
* test.txt for ancillary
* test.pcap for rtp pcap models
* test_st22.pcap for st22 pcap files

## 2. Loop test

Loop test with all json files under loop_json dir.

### 2.1. Customize the port and IP(Optional)

```bash
cd loop_json/
```

Edit the port in change_port.sh as the setup, then run:

```bash
./change_port.sh
```

Use the random IP for json config file, run:

```bash
./random_ip.sh
```

### 2.2. Run the loop test

```bash
./loop_test.sh
```

Run below cmd for the software timing parser test with loop mode.

```bash
./loop_TP_test.sh
```

## 3. AFXDP loop test

AFXDP loop test with all json files under afxdp_json dir.

### 3.1. Setup the env(Optional)

```bash
cd afxdp_json/
```

Edit the setup.sh as the setup and also the json files, then run:

```bash
./setup.sh
```

### 3.2. Run the AFXDP loop test

```bash
./afxdp_test.sh
```

## 4. Dual core redundant test

```bash
./redundant_test.sh
```

## 5. Header split test(PF PMD mode only)

### 5.1. Bind to PF pmd mode with root

```bash
export PATH=$PATH:/usr/local/bin/
../../script/nicctl.sh bind_pmd 0000:af:00.0
../../script/nicctl.sh bind_pmd 0000:af:00.1
```

### 5.2. Run the test with loop json

```bash
./hdr_split_test.sh
```

## 6. Sample test

Customize sample_test.sh as the setup and run

```bash
./sample_test.sh
```
