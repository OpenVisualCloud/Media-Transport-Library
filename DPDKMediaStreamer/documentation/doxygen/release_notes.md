@page release_notes Release Notes

@section release_notes_beta_1_sec Beta 1

@subsection release_notes_beta_1_subsec_1 1. Features

* Transmit/Receive Raw Video Frames in HD-SDI format (Pixel format: YUV 4:2:2 10bit)
    * Tested video formats are: 1080p59 and 1080p50.
    * Early support for 1080p29, 1080p25.
* Transmit/Receive Raw Audio Frames and Ancillary data.
* Support for 10/25/40 Gb/s Intel Fortville Network Interface Cards.
* Supports transmit and receive for unicast and multicast.
* Time synchronization using PTP (Precision Time Protocol).
* Supports IGMP v2 and selected v3 functionalities.
	* ST 2110 protocols suite supported:
	* ST 2110-20
	* ST 2110-21 (experimental)
	* ST 2110-30
	* ST 2110-40
	* ST 2022-7

@subsection release_notes_beta_1_subsec_2 2. API Changes

@subsection release_notes_beta_1_subsec_3 3. Tested Platforms

Platform description | NIC
--- | ---
Intel(R) Xeon(R) Platinum 8260M CPU @ 2.40GHz (Cascade Lake), 507GB RAM | Intel® Ethernet Controller X710, 10 GbE
Intel(R) Xeon(R) Gold 6140 CPU @ 2.30GHz (Skylake), 95GB RAM | Intel® Ethernet Controller X710, 25 GbE
Intel(R) Xeon(R) Gold 6140M CPU @ 2.30GHz  (Skylake), 257GB RAM | Intel® Ethernet Controller X710, 40 GbE


@subsection release_notes_beta_1_subsec_4 4. Known issues

* Library outputs a log “Conflicting rules exist”. 

    ***Workaround:*** *This is known DPDK PMD logging issue for Fortville NIC. RTE FLOW rules are applied hence no fix must be provided.*
* Memory Leak occurs after creating and destroying TX session.

    ***Workaround:*** *None. Will be investigated.*
* CreateSession() API cannot be called twice on running application.

    ***Workaround:*** *None. Will be investigated.*
* At the beginning of the session bad audio rewinding errors appear. After a while this issue is not observed.

    ***Workaround:*** *None. Will be investigated.*
* IGMP v3 packet "leave group" is not sent from RX app during application exit.

    ***Workaround:*** *None. As per IGMP v3 standard “leave group” packet is optional. Under investigation for next release.*
* Incorrect behavior of dual <TX &RX> mode of application.

    ***Workaround:*** *Please start app as either TX or RX (not both). Under investigation for next release.*
* Send and Receive on DPDK KNI with virtual IP is not working.

    ***Workaround:*** *None. Will be investigated.*
* Maximum number of 8 ST2110-20 Video session format 1080p59 as supported and verified.

    ***Workaround:*** *None.*
* Library does not select free or underutilized performance cores.

    ***Workaround:*** *Select performance cores and ensure no other applications are running with help of grub isol (as documented in README.md).*

@section release_notes_beta_1_1_sec Beta 1.1

@subsection release_notes_beta_1_1_subsec_1 1. Features

@subsection release_notes_beta_1_1_subsec_2 2. API Changes

@subsection release_notes_beta_1_1_subsec_3 3. Tested Platforms

@subsection release_notes_beta_1_1_subsec_4 4. Known issues