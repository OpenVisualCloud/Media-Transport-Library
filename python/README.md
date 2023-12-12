# The Python support

IMTL leverage SWIG, found at <https://github.com/swig/swig/tree/master>, to transform C APIs into a binding layer that Python can utilize.

## 1. Build and install swig

Following the build and installation guide from the SWIG GitHub repository at <https://github.com/swig/swig/tree/master>.

Below are the example steps to build the `v4.1.1` release. Replace the tag with a newer one if there is a more recent release of SWIG available:

```bash
git clone https://github.com/swig/swig.git
cd swig/
git checkout v4.1.1
git log
./autogen.sh
./configure
make
sudo make install
```

## 2. Build and install IMTL python binding layer

### 2.1 Create IMTL binding layer code based on swig

```bash
cd $imtl_source_code/python/swig/
swig -python -I/usr/local/include pymtl.i
```

If you encounter the error `mtl.i:15: Error: Unable to find 'mtl/mtl_api.h'`, this is typically due to an incorrect include path. Use the following command to locate the correct path for `mtl_api.h`:

```bash
find /usr/ -name mtl_api.h
```

Once you have obtained the correct path, you can update your SWIG interface file or your build configuration to reference the correct location of `mtl_api.h`.

### 2.2 Build

```bash
python3 setup.py build_ext --inplace
```

### 2.3 Install

```bash
sudo python3 setup.py install
```

Checking the log to see the path installed.

```bash
creating /usr/local/lib/python3.10/dist-packages/pymtl-0.1-py3.10-linux-x86_64.egg
```

## 3. Run python example code

```bash
cd $imtl_source_code/python/example/
python3 ../example/sample.py
```
