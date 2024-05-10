#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

set -e

if [ $# -lt 1 ]; then
	echo "Please specify the network interface"
	exit 0
fi

if_name=$1

echo "Start to delete all filters for $if_name"
for rule in $(ethtool -n "$if_name" | grep 'Filter:' | awk '{print $2}'); do
	echo "Delete filter $rule"
	ethtool -N "$if_name" delete "$rule"
done
