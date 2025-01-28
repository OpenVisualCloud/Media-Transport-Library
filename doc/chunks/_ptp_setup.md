<!-- markdownlint-disable MD001 MD041 -->
The Precision Time Protocol (PTP) facilitates global timing accuracy in the microsecond range for all essences.
Typically, a PTP grandmaster is deployed within the network, and clients synchronize with it using tools like ptp4l.
This library includes its own PTP implementation, and a sample application offers the option to enable it.
Please refer to section [Built-in PTP](#built-in-ptp) for instructions on how to enable it.

By default, the built-in PTP feature is disabled, and the PTP clock relies on the system time source of the user application (clock_gettime). However, if the built-in PTP is enabled, the internal NIC time will be selected as the PTP source.

#### Linux ptp4l Setup to Sync System Time with Grandmaster

Firstly run ptp4l to sync the PHC time with grandmaster, customize the interface as your setup.

```bash
sudo ptp4l -i ens801f2 -m -s -H
```

Then run phc2sys to sync the PHC time to system time, please make sure NTP service is disabled as it has conflict with phc2sys.

```bash
sudo phc2sys -s ens801f2 -m -w
```

#### Built-in PTP

This project includes built-in support for the Precision Time Protocol (PTP) protocol, which is also based on the hardware Network Interface Card (NIC) timesync feature. This combination allows for achieving a PTP time clock source with an accuracy of approximately 30ns.

To enable this feature in the RxTxApp sample application, use the `--ptp` argument. The control for the built-in PTP feature is the `MTL_FLAG_PTP_ENABLE` flag in the `mtl_init_params` structure.

Note: Currently, the VF (Virtual Function) does not support the hardware timesync feature. Therefore, for VF deployment, the timestamp of the transmitted (TX) and received (RX) packets is read from the CPU TSC (TimeStamp Counter) instead. In this case, it is not possible to obtain a stable delta in the PTP adjustment, and the maximum accuracy achieved will be up to 1us.
