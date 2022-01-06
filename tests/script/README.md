# loop test script

## 1. Customize the port and IP.
Edit the port in change_port.sh as the setup, then run: (Optional)
```bash
./change_port.sh
```
Use the random IP for json config file, run:
```bash
git checkout *.json
./random_ip.sh
```

## 2. Prepare the test resource files, copy to current folder.
* test.yuv for 1080p video
* test_720p.yuv for 720p video
* test_4k.yuv for 4k video
* test.wav for audio
* test.txt for ancillary
* test.pcap for 1080p rtp pcap

## 3. Run the test:
```bash
./auto_loop_test.sh
```
```bash
./auto_ebu_test.sh
```
