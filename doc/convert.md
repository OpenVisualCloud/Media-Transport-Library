# Color Format Convert Guide

## Introduction

Starting from version 22.06, the Intel® Media Transport Library introduces a color format SIMD convert API, which can be used to convert between RFC4175 YUV422 10-bit BE and common LE formats.

### The SIMD API

The library supports SIMD flags detection at runtime.

The default function `st20_<src_format>_to_<dest_format>` will attempt to use the maximum SIMD level supported on the platform. Users can also specify the maximum SIMD level by calling `st20_<src_format>_to_<dest_format>_simd`. 

For detailed API usage, please refer to [st_convert_api.h](../include/st_convert_api.h) and [st_convert_internal.h](../include/st_convert_internal.h).

### The DMA Helper API

During the conversion of ultra-high-definition video frames, the LLC load miss rate can be high due to wide-ranging memory access. To reduce the LLC load miss, we have introduced a DMA helper for the convert API. The source data is preloaded into the software cache blocks with the DMA engine before the SIMD batch processing, so the SIMD load functions can always hit the cache. This API is implemented with synchronous dma_copy, so the conversion speed is not always optimized. It is particularly helpful for 4K or 8K scenarios.

To use the functions `st20_<src_format>_to_<dest_format>_dma`, you need to first acquire the DMA device and pass the st_udma_handle to the function. For information on DMA device creation, please refer to [dma_sample.c](../app/sample/dma_sample.c).

For detailed API usage, please refer to [st_convert_api.h](../include/st_convert_api.h).

## Supported Conversion

### 4:2:2 10 bits

| src_format| dest_format | scalar | avx2 | avx512 | avx512_vbmi |
| :---      |     :---    | :----: |:----:| :----: |    :----:   |
| rfc4175_422be10   | yuv422p10le       | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422be10   | rfc4175_422le10   | &#x2705; | &#x2705; | &#x2705; | &#x2705; |
| rfc4175_422be10   | v210              | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422be10   | y210              | &#x2705; |          | &#x2705; |          |
| rfc4175_422be10   | rfc4175_422le8    | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422le10   | v210              | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422le10   | rfc4175_422be10   | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422le10   | yuv422p10le       | &#x2705; |          |          |          |
| yuv422p10le       | rfc4175_422be10   | &#x2705; |          | &#x2705; |          |
| yuv422p10le       | rfc4175_422le10   | &#x2705; |          |          |          |
| v210              | rfc4175_422be10   | &#x2705; |          | &#x2705; | &#x2705; |
| y210              | rfc4175_422be10   | &#x2705; |          | &#x2705; |          |

### 4:2:2 12 bits

| src_format| dest_format | scalar | avx2 | avx512 | avx512_vbmi |
| :---      |     :---    | :----: |:----:| :----: |    :----:   |
| rfc4175_422be12   | yuv422p12le       | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422be12   | rfc4175_422le12   | &#x2705; |          | &#x2705; |          |
| rfc4175_422le12   | yuv422p12le       | &#x2705; |          |          |          |
| rfc4175_422le12   | rfc4175_422be12   | &#x2705; |          |          |          |
| yuv422p12le       | rfc4175_422be12   | &#x2705; |          |          |          |
| yuv422p12le       | rfc4175_422le12   | &#x2705; |          |          |          |

### 4:4:4 10 bits

| src_format| dest_format | scalar | avx2 | avx512 | avx512_vbmi |
| :---      |     :---    | :----: |:----:| :----: |    :----:   |
| rfc4175_444be10   | yuv444p10le       | &#x2705; |          |          |          |
| rfc4175_444be10   | gbrp10le          | &#x2705; |          |          |          |
| rfc4175_444be10   | rfc4175_444le10   | &#x2705; |          |          |          |
| rfc4175_444le10   | yuv444p10le       | &#x2705; |          |          |          |
| rfc4175_444le10   | gbrp10le          | &#x2705; |          |          |          |
| rfc4175_444le10   | rfc4175_444be10   | &#x2705; |          |          |          |
| yuv444p10le       | rfc4175_444be10   | &#x2705; |          |          |          |
| yuv444p10le       | rfc4175_444le10   | &#x2705; |          |          |          |
| gbrp10le          | rfc4175_444be10   | &#x2705; |          |          |          |
| gbrp10le          | rfc4175_444le10   | &#x2705; |          |          |          |

### 4:4:4 12 bits

| src_format| dest_format | scalar | avx2 | avx512 | avx512_vbmi |
| :---      |     :---    | :----: |:----:| :----: |    :----:   |
| rfc4175_444be12   | yuv444p12le       | &#x2705; |          |          |          |
| rfc4175_444be12   | gbrp12le          | &#x2705; |          |          |          |
| rfc4175_444be12   | rfc4175_444le12   | &#x2705; |          |          |          |
| rfc4175_444le12   | yuv444p12le       | &#x2705; |          |          |          |
| rfc4175_444le12   | gbrp12le          | &#x2705; |          |          |          |
| rfc4175_444le12   | rfc4175_444be12   | &#x2705; |          |          |          |
| yuv444p12le       | rfc4175_444be12   | &#x2705; |          |          |          |
| yuv444p12le       | rfc4175_444le12   | &#x2705; |          |          |          |
| gbrp12le          | rfc4175_444be12   | &#x2705; |          |          |          |
| gbrp12le          | rfc4175_444le12   | &#x2705; |          |          |          |

## Formats For Reference

### rfc4175_422le10

Color space: YUV (YCbCr)<br>
Sample: 422<br>
Packed/planar: packed<br>
Depth: 10<br>
Bytes/pixels: 5/2<br>
Endian: LE<br>
Memory Layout:<br>
UYVY10bit LE (1 pixel group)<br>

```bash
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  C'B00 (10 bits)  |   Y'00 (10 bits)  |  C'R00 (10 bits)  |   Y'01 (10 bits)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |      0x4      |
```

### rfc4175_422be10

Color space: YUV<br>
Sample: 422<br>
Packed/planar: packed<br>
Depth: 10<br>
Bytes/pixels: 5/2<br>
Endian: BE<br>
Memory Layout:<br>
UYVY10bit BE (1 pixel group)<br>

```bash
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       U       |     Y0    | U_|   V   |  Y0_  | Y1|     V_    |      Y1_      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |      0x4      |
```

### rfc4175_422le12

Color space: YUV (YCbCr)<br>
Sample: 422<br>
Packed/planar: packed<br>
Depth: 12<br>
Bytes/pixels: 6/2<br>
Endian: LE<br>
Memory Layout:<br>
UYVY12bit LE (1 pixel group)<br>

```bash
 0                       1                       2                       3
 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    C'B00 (12 bits)    |     Y'00 (12 bits)    |    C'R00 (12 bits)    |     Y'01 (12 bits)    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |      0x4      |      0x5      |
```

### rfc4175_422be12

Color space: YUV<br>
Sample: 422<br>
Packed/planar: packed<br>
Depth: 12<br>
Bytes/pixels: 6/2<br>
Endian: BE<br>
Memory Layout:<br>
UYVY12bit BE (1 pixel group)<br>

```bash
 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       U       |   Y0  |   U_  |      Y0_      |       V       |   Y1  |   V_  |      Y1_      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |      0x4      |      0x5      |
```

### rfc4175_444le10

Color space: YUV (YCbCr) or RGB<br>
Sample: 444<br>
Packed/planar: packed<br>
Depth: 10<br>
Bytes/pixels: 15/4<br>
Endian: LE<br>
Memory Layout:<br>
UYVY10bit/RGB10bit LE (1 pixel group)<br>

```bash
 0                   1                   2                   3                   4                   5                   6                   7                   8                   9                   10                  11
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  CB_R00 (10 bits) |  Y_G00 (10 bits)  |  CR_B00 (10 bits) |  CB_R01 (10 bits) |  Y_G01 (10 bits)  |  CR_B01 (10 bits) |  CB_R02 (10 bits) |  Y_G02 (10 bits)  |  CR_B02 (10 bits) |  CB_R03 (10 bits) |  Y_G03 (10 bits)  |  CR_B03 (10 bits) |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |      0x4      |      0x5      |      0x6      |      0x7      |      0x8      |      0x9      |      0xA      |      0xB      |      0xC      |      0xD      |      0xE      |
```

### rfc4175_444le12

Color space: YUV (YCbCr) or RGB<br>
Sample: 444<br>
Packed/planar: packed<br>
Depth: 12<br>
Bytes/pixels: 9/2<br>
Endian: LE<br>
Memory Layout:<br>
UYVY12bit/RGB12bit LE (1 pixel group)<br>

```bash
 0                       1                       2                       3                       4                       5
 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B 0 1 2 3 4 5 6 7 8 9 A B
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    CB_R00 (12 bits)   |    Y_G00 (12 bits)    |    CR_B00 (12 bits)   |    CB_R01 (12 bits)   |    Y_G01 (12 bits)    |    CR_B01 (12 bits)   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |      0x4      |      0x5      |      0x6      |      0x7      |      0x8      |
```

### v210

Color space: YUV<br>
Sample: 422<br>
Packed/planar: packed<br>
Depth: 10<br>
Bytes/pixels: 16/6<br>
Endian: LE<br>
Memory Layout:<br>
V210 (3 pixel groups)<br>

```bash
 0                   1                   2                       
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9     
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  C'B00 (10 bits)  |   Y'00 (10 bits)  |  C'R00 (10 bits)  |0 0|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
 3                   4                   5
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Y'01 (10 bits)  |  C'B01 (10 bits)  |   Y'02 (10 bits)  |0 0|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x4      |      0x5      |      0x6      |      0x7      |
 6                   7                   8
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  C'R01 (10 bits)  |   Y'03 (10 bits)  |  C'B02 (10 bits)  |0 0|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x8      |      0x9      |      0xA      |      0xB      |
 9                   A                   B
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Y'04 (10 bits)  |  C'R02 (10 bits)  |   Y'05 (10 bits)  |0 0|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0xC      |      0xD      |      0xE      |      0xF      |
```

### rfc4175_422le8

Color space: YUV<br>
Sample: 422<br>
Packed/planar: packed<br>
Depth: 8<br>
Bytes/pixels: 4/2<br>
Endian: LE<br>
Memory Layout:<br>
UYVY LE (1 pixel group)<br>

```bash
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     C'B00     |      Y'00     |     C'R00     |      Y'01     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
```

### yuv422p10le

Color space: YUV<br>
Sample: 422<br>
Packed/planar: planar<br>
Depth: 10<br>
Bytes/pixels: 8/2<br>
Endian: LE<br>
Memory Layout:<br>
YUV42210bitPlanar LE<br>

```bash
Y channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Y'00 (10 bits)  |    pad    |   Y'01 (10 bits)  |    pad    |   
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      | 

U channel (w*h):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   U'00 (10 bits)  |    pad    |   U'01 (10 bits)  |    pad    |   
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      | 

V channel (w*h):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   V'00 (10 bits)  |    pad    |   V'01 (10 bits)  |    pad    |   
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
```

### y210

Color space: YUV<br>
Sample: 422<br>
Packed/planar: packed<br>
Depth: 10<br>
Bytes/pixels: 8/2<br>
Endian: LE<br>
Memory Layout:<br>
Y210 (1 pixel group)

```bash
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    pad    |   Y'00 (10 bits)  |    pad    |   U'00 (10 bits)  |   
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      | 
 2                               3
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    pad    |   Y'01 (10 bits)  |    pad    |   V'00 (10 bits)  |   
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x4      |      0x5      |      0x6      |      0x7      | 
```

### yuv422p12le

Color space: YU<br>
Sample: 422<br>
Packed/planar: planar<br>
Depth: 12<br>
Bytes/pixels: 8/2<br>
Endian: LE<br>
Memory Layout:<br>
YUV42212bitPlanar LE<br>

```bash
Y channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Y'00 (12 bits)    |  pad  |     Y'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

U channel (w*h):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     U'00 (12 bits)    |  pad  |     U'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

V channel (w*h):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     V'00 (12 bits)    |  pad  |     V'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
```

### yuv444p10le

Color space: YUV<br>
Sample: 444<br>
Packed/planar: planar<br>
Depth: 10<br>
Bytes/pixels: 8/2<br>
Endian: LE<br>
Memory Layout:<br>
YUV44410bitPlanar LE<br>

```bash
Y channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Y'00 (10 bits)  |    pad    |   Y'01 (10 bits)  |    pad    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

U channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   U'00 (10 bits)  |    pad    |   U'01 (10 bits)  |    pad    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

V channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   V'00 (10 bits)  |    pad    |   V'01 (10 bits)  |    pad    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
```

### yuv444p12le

Color space: YUV<br>
Sample: 444<br>
Packed/planar: planar<br>
Depth: 12<br>
Bytes/pixels: 8/2<br>
Endian: LE<br>
Memory Layout:<br>
YUV44412bitPlanar LE<br>

```bash
Y channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Y'00 (12 bits)    |  pad  |     Y'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

U channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     U'00 (12 bits)    |  pad  |     U'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

V channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     V'00 (12 bits)    |  pad  |     V'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
```

### gbrp10le

Color space: RGB<br>
Sample: 444<br>
Packed/planar: planar<br>
Depth: 10<br>
Bytes/pixels: 8/2<br>
Endian: LE<br>
Memory Layout:<br>
GBR10bitPlanar LE<br>

```bash
Y channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   G'00 (10 bits)  |    pad    |   G'01 (10 bits)  |    pad    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

U channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   B'00 (10 bits)  |    pad    |   B'01 (10 bits)  |    pad    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

V channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   R'00 (10 bits)  |    pad    |   R'01 (10 bits)  |    pad    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
```

### gbrp12le

Color space: RGB<br>
Sample: 444<br>
Packed/planar: planar<br>
Depth: 12<br>
Bytes/pixels: 8/2<br>
Endian: LE<br>
Memory Layout:<br>
GBR12bitPlanar LE<br>

```bash
Y channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     B'00 (12 bits)    |  pad  |     B'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

U channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     B'00 (12 bits)    |  pad  |     B'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |

V channel (w*h*2):
 0                               1
 0 1 2 3 4 5 6 7 8 9 A B C D E F 0 1 2 3 4 5 6 7 8 9 A B C D E F
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     R'00 (12 bits)    |  pad  |     R'01 (12 bits)    |  pad  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
```
