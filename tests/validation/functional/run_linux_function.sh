#!/bin/bash

for i in $(cat linux_function_case_list.txt)
do
	echo "$i"
	python2 Execut.py -t kahawai_function_test -c $i >> run.log 2>&1

	mv logs logs_$i
done
