#!/bin/bash

while IFS= read -r line; do
	echo "$line"
	python Execut.py -t function_kahawai_invalid -c "$line" >>run.log 2>&1

	mv logs logs_"$line"
done <invalid_case_list.txt
