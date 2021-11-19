@page sol_over Solution Overview

@section det_arch Solution overview
\n
Architecture of Intel® ST 2110 Media Streaming Library is based on a two sided communication scheme. There are three major parts to create a whole solution. 
1.  Application part <span style="color:green"><b>(green color blocks)</b></span>
2.  Library part <span style="color:blue"><b>(blue color blocks)</b></span>
3.  Hardware part managed by DPDK framework (<span style="color:orange"><b>orange</b></span>, <span style="color:gray"><b>gray</b></span> and <span style="color:red"><b>red</b></span> coloured blocks)

Aplication mode:
* Producer mode for each session (video, audio or ancillary) content
* Consumer mode for processing the received essence content (frame for video, signal for audio, and different data for ancillary)
* Dual (consumer + producer) mode recieves content form various essences to be forwarded as post processed content

<b>NOTE: Custom mode of user timestamp supported.</b>

Library functionalities:
* Filter traffic for ST 2110 and other types
* Filter and process ARP messages associated to ST 2110 of IPv4 addresses
* PTP processing
* IGMP v3 support and IGMP querier
* Scheduling and pacing supported in the software
* IP, RTP and payload verification


\image html detailedArchitecture.png "Architecture" width=60%

<b>NOTE:\n </b>
<b> Library is based on DPDK kernel bypass library and poll mode driver (PMD) using Intel foundational NICs.\n</b>

\n
@section over_arch Setup

Basic configuration of the environment is shown on the scheme below. The configuration consists of a single transmitter and a single receiver when used with unicast communication, and one or more transmitters and one or more receivers in multicast communication. To work properly (with proper time synchronization), the environment must have PTP clock enabled in the network. 

\image html setup.png "Example setup" width=50%
\n
\n
@section software_deployment_model Deployment Model

Single application process that uses ST2110 library which inherits DPDK. Single application assigns 2 ports (PCIe devices) to a ST device that is then populated with sessions. The application can perform ST2110 receive, transmit or both.

\image html softwareDeploymentModel.png "Deployment Model" width=40%

