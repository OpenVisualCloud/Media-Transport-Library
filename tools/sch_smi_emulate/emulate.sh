#!/bin/bash

if [ -z "$1" ] || [ -z "$2" ] || [[ $2 -lt 0 ]] || [[ $2 -gt 100 ]]; then
    echo "Usage: $0 <number_of_emulators> <percantage_of_smi>"
    exit 1
fi

sleep_ms=$((100 - $2))
work_us=$(($2 * 1000))

pids=()

if [ ! -x ./sch_smi_emulate ]; then
    echo "Error: ./sch_smi_emulate not found or not executable."
    exit 1
fi

for ((i=0; i<$1; i++)); do
    ./sch_smi_emulate --sleep_ms $sleep_ms --work_us $work_us &
    pids+=($!)
done

trap 'kill "${pids[@]}" 2>/dev/null; exit' SIGINT

wait

