#!/bin/bash

#for i in Cvt Dma-dma all_test all_test-dma all_test-dma_runtime all_test-runtime Main Misc St20_rx-dma St20_tx St22p St22_rx-dma St22_tx St30_rx-dma St30_tx St40_rx-dma St40_tx
for i in $(cat API_case_list.txt)
do
	echo "i"
	python Execut.py -t function_kahawai_api -c $i

	mv logs logs_$i
done
