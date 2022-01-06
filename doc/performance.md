# Performance Benchmark for CVL

## 1. benchmark server setup:
Server Model: Intel Corporation S2600WFT  
Processor Information: Intel(R) Xeon(R) Gold 6252N CPU @ 2.30GHz
                       2 Processor, 24 Cores  
Memory: 12 x 16G (DIMM DDR4 Synchronous 3200 MHz)  
NIC: CVL 100G  
APP used: RxTxApp reference app.

## 2. benchmark result:
#### 2.1 transmitter frame mode
sch_session_quota is default 10.
we could make adjustment in st_init by data_quota_mbs_per_sch

data_quota_mbs_per_sch = sch_session_quota * 2589 + 100.
1 schedule needs one lcore in lib.

following tables show the max-session for different schedulers

max video tx session is limited to 60 now

schedules | 1080p29.97 | 1080p59.94 | 1080p50 |  2160p29.97 | 2160p59.94 | 2160p50 
 --- | --- | --- | --- | --- | --- |---
 1 |  20  | 10 | 12 | 5 | 2 | 3
 2  | 36 | 20 | 24 | 10 | 4 | 6
 3 | 48  | 27 | 30 | 15 | 6 | 9
 4 | 60  | 32 | 36 | 16 | 8 | 11

following tables show sch_session_quota setting to get max-session for different schedulers

schedules | 1080p29.97 | 1080p59.94 | 1080p50 |2160p29.97 | 2160p59.94 | 2160p50
 --- | --- | --- | --- | --- | --- | ---
 1 |  10  | 10 | 10 | 10 | 10 | 10
 2  | 9 | 10 | 10 | 10 | 10 | 10
 3 | 8  | 9 | 9 | 10 | 10 | 10
 4 | 8  | 8 | 8 | 9 | 10 | 10
