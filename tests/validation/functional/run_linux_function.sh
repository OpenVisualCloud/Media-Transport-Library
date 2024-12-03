#!/bin/bash

while IFS= read -r line
do
	echo "$line"
	python2 Execut.py -t kahawai_function_test -c "$line" >> run.log 2>&1

	mv logs logs_"$line"
done < linux_function_case_list.txt
