@mainpage Overview

@section intro Introduction
ST 2110 is replacement for SDI which allows for better transmission and reception of various essence streams (video, audio, ancillary) across an IP network.
\n
\n
\image HTML streamsOverview.png "Streams Overview" width=60%
\n
\n
The Media Transport Library is a solution based on DPDK (Data Plane Development Kit) for transmitting and receiving raw video. Raw video RTP (real time protocol) is based on the SMPTE ST 2110-21 standard. This solution is able to process high quality, full HD streams, without encoding. Connections with "no compression" and high speed network packets processing (utilizing DPDK) provides high quality and low latency transmission.
\n
\n
\image HTML EssenceStreams.png "Essence Streams" width=70%
\n
\n
@section intro_sub_1 Library supported features

Standard | Description
--- | ---
ST 2110-10-2017 |   System Timing and Definitions
ST 2110-20-2017 |   Uncompressed Active Video
ST 2110-21-2017 |   Traffic Shaping and Delivery Timing for Video
ST 2110-30-2017 |   PCM Digital Audio
ST 2110-40-2018 |   Ancillary Data
ST 2022-7-2013  |   Seamless Protection Switching of SMPTE ST 2022 IP Datagrams
RFC 826         |   Address Resolution Protocol (ARP)
RFC 3376 and RFC 2236       |   Internet Group Management Protocol (IGMP), Version 3

NOTE: The library supports transmission of the three essence streams:

* Video
* Audio
* Ancillary data
