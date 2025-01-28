<!-- markdownlint-disable MD001 MD041 -->
```bash
cd src
make
sudo make install
# sudo rmmod irdma 2>/dev/null
sudo rmmod ice
sudo modprobe ice
cd -
```
