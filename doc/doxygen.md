# doxygen guide for APIs

## 1. Install the doxygen tools

#### 1.1 Ubuntu/Debian

```bash
sudo apt-get install doxygen
```

#### 1.2 Centos

```bash
sudo yum install doxygen
```

## 2. Build

Just run below command in the top tree of the Kahawai project, then check doxygen/html/index.html for the doxygen documents.

```bash
cd doc/doxygen
rm doc/html/ -rf
source build-doc.sh
```
