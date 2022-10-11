# Color Format Convert Guide

## Introduction
Kahawai introduces color format SIMD convert API from v22.06, which can be used to conversion between RFC4175 YUV422 10bit BE and common LE formats.  
### The SIMD API
Kahawai supports SIMD flags detection both in compile stage and runtime.   
The default function `st20_<src_format>_to_<dest_format>` will try to use the maximum SIMD level supported on the platform, and user can also specify the maximum SIMD level by calling `st20_<src_format>_to_<dest_format>_simd`. 
For full API usage please refer to [st_convert_api.h](../include/st_convert_api.h) and [st_convert_internal.h](../include/st_convert_internal.h).
### The DMA Helper API
While converting ultra high definition video frames, the LLC load miss rate will be high due to wide rage memory access. To reduce the LLC load miss, Kahawai introduces DMA helper for the convert API. The source date is preloaded to the software cache blocks with DMA engine before the SIMD batch processing, so the SIMD load functions can always hit the cache. This API is implemented with synchronous dma_copy, so the conversion speed is not always optimized. It is helpful for 4K or 8K senario.   
To use the functions `st20_<src_format>_to_<dest_format>_dma`, the DMA device needs to be aquired first and st_udma_handle is passed here. For DMA device creation please refer to [dma_sample.c](../app/sample/dma_sample.c).
For full API usage please refer to [st_convert_api.h](../include/st_convert_api.h).

## Supported Conversion
| src_format| dest_format | scalar | avx2 | avx512 | avx512_vbmi |
| :---      |     :---    | :----: |:----:| :----: |    :----:   |
| rfc4175_422be10   | yuv422p10le       | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422be10   | rfc4175_422le10   | &#x2705; | &#x2705; | &#x2705; | &#x2705; |
| rfc4175_422be10   | v210              | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422be10   | y210              | &#x2705; |          | &#x2705; |          |
| rfc4175_422be10   | rfc4175_422le8    | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422le10   | yuv422p10le       | &#x2705; |          |          |          |
| rfc4175_422le10   | v210              | &#x2705; |          | &#x2705; | &#x2705; |
| rfc4175_422le10   | rfc4175_422be10   | &#x2705; |          | &#x2705; | &#x2705; |
| yuv422p10le       | rfc4175_422be10   | &#x2705; |          | &#x2705; |          |
| v210              | rfc4175_422be10   | &#x2705; |          | &#x2705; | &#x2705; |
| y210              | rfc4175_422be10   | &#x2705; |          | &#x2705; |          |

## Formats For Reference
### rfc4175_422le10
Color space: YUV (YCbCr)  
Sample: 422  
Packed/planar: packed  
Depth: 10  
Bytes/pixels: 5/2  
Endian: LE  
Memory Layout:  
UYVY10bit LE (1 pixel group)  
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  C'B00 (10 bits)  |   Y'00 (10 bits)  |  C'R00 (10 bits)  |   Y'01 (10 bits)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |      0x4      |
```

### rfc4175_422be10
Color space: YUV  
Sample: 422  
Packed/planar: packed  
Depth: 10  
Bytes/pixels: 5/2  
Endian: BE  
Memory Layout:  
UYVY10bit BE (1 pixel group)  
```
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       U       |     Y0    | U_|   V   |  Y0_  | Y1|     V_    |      Y1_      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |      0x4      |
```

### v210
Color space: YUV  
Sample: 422  
Packed/planar: packed  
Depth: 10  
Bytes/pixels: 16/6  
Endian: LE  
Memory Layout:  
V210 (3 pixel groups)  
```
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
Color space: YUV  
Sample: 422  
Packed/planar: packed  
Depth: 8  
Bytes/pixels: 4/2  
Endian: LE  
Memory Layout:  
UYVY LE (1 pixel group)  
```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     C'B00     |      Y'00     |     C'R00     |      Y'01     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      0x0      |      0x1      |      0x2      |      0x3      |
```

### yuv422p10le
Color space: YUV  
Sample: 422  
Packed/planar: planar  
Depth: 10  
Bytes/pixels: 8/2  
Endian: LE  
Memory Layout:  
YUV42210bitPlanar LE  
```
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
Color space: YUV  
Sample: 422  
Packed/planar: packed  
Depth: 10  
Bytes/pixels: 8/2  
Endian: LE  
Memory Layout:  
Y210 (1 pixel group)
```
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
