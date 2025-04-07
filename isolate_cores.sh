#!/bin/bash


if [ -z "$CPUS" ]; then
    CPUS="1,3,5,7,11,13,15,17,19"
fi

if [ "$1" == "unset" ]; then
    echo "Unsetting CPU isolation for ${CPUS}"

    sudo grubby --update-kernel=ALL --remove-args="isolcpus=${CPUS}"
    echo sudo grubby --update-kernel=ALL --remove-args="isolcpus=${CPUS}"
    echo
    sudo grubby --update-kernel=ALL --remove-args="nohz_full=${CPUS}"
    echo sudo grubby --update-kernel=ALL --remove-args="nohz_full=${CPUS}"
    echo
    sudo grubby --update-kernel=ALL --remove-args="rcu_nocbs=${CPUS}"
    echo sudo grubby --update-kernel=ALL --remove-args="rcu_nocbs=${CPUS}"
    echo

else
    echo sudo grubby --update-kernel=ALL --remove-args="isolcpus=${CPUS}"
    sudo grubby --update-kernel=ALL --args="isolcpus=${CPUS}"
    echo
    echo sudo grubby --update-kernel=ALL --remove-args="nohz_full=${CPUS}"
    sudo grubby --update-kernel=ALL --args="nohz_full=${CPUS}"
    echo
    echo sudo grubby --update-kernel=ALL --remove-args="rcu_nocbs=${CPUS}"
    echo
    sudo grubby --update-kernel=ALL --args="rcu_nocbs=${CPUS}"

    echo "CPU isolation set to ${CPUS}"
fi


