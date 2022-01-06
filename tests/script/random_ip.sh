#!/bin/bash

set -e

SUB1=$[$RANDOM%256]
SUB2=$[$RANDOM%256]

echo "Change IP to $SUB1.$SUB2"

sed -i 's/.168.17./.'"$SUB1"'.'"${SUB2}"'./g' *.json
