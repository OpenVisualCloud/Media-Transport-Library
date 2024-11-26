#!/bin/bash

for i in $(cat invalid_case_list.txt)
do
	echo "$i"
	python Execut.py -t function_kahawai_invalid -c $i >> run.log 2>&1

	mv logs logs_$i
done
