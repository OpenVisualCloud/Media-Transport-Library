/*
* Copyright 2020 Intel Corporation.
*
* This software and the related documents are Intel copyrighted materials,
* and your use of them is governed by the express license under which they
* were provided to you ("License").
* Unless the License provides otherwise, you may not use, modify, copy,
* publish, distribute, disclose or transmit this software or the related
* documents without Intel's prior written permission.
*
* This software and the related documents are provided as is, with no
* express or implied warranties, other than those that are expressly stated
* in the License.
*
*/

/*
  *
  *    Transmitting and receiving example using Media streamer based on DPDK
  *
  */

#include "rxtx_app.h"

#include "rx_view.h"

typedef struct st_user_params
{
	// Input Parameters
	int rxOnly;
	int txOnly;
	bool isEbuCheck;
	uint8_t ipAddr[4];	/**< destination IP */
	uint8_t sipAddr[4]; /**< sender IP */
	uint8_t macAddr[6]; /**< destination MAC */
	uint32_t rate;
	uint32_t interlaced;
	uint32_t fmtIndex;
	uint16_t udpPort;
	uint32_t snCount;
	st21_buf_fmt_t bufFormat;

	char *outPortName;
	char *inPortName;

} st_user_params_t;

void
ShowWelcomeBanner()
{
	printf("\n#################################################\n");
	printf("#                                               #\n");
	printf("#     Intel ST 2110 Media Streaming Library     #\n");
	printf("#        Sample Send/Receive application        #\n");
	printf("#                                               #\n");
	printf("#################################################\n\n");
}

void
PrintVersion()
{
	st_param_val_t val;
	printf("\n");
	printf("API version: %d.%d.%d\n", ST_VERSION_MAJOR_CURRENT, ST_VERSION_MINOR_CURRENT,
		ST_VERSION_LAST);

	StGetParam(ST_LIB_VERSION, &val);
	printf("Library version: %s\n", val.strPtr);
#ifdef GIT
	printf("Git commit ID: " GIT "\n");
#endif
	StGetParam(ST_BUILD_ID, &val);
	if (val.valueU64 != 0)
		printf("Build version: %ld\n", val.valueU64);
	else
		printf("Build version: --no build version available--\n");

	printf("\n");
}

void
PrintHelp()
{
	printf("\n");
	printf("##### Usage: #####\n\n");
	printf(" Params:\n");
	printf("   -h                                           : print this help info \n");
	printf("   -v                                           : print versions info \n");
	printf("   --mac <MAC addr>                             : destination MAC address \n");
	printf("   --ip <IP addr>                               : destination IP address \n");
	printf("   --sip <IP addr>                              : source IP address \n");
	printf("   --ebu                                        : enable EBU compatibility with standard ST 2110 "
		"logs\n");
	printf("   -p <UDP port> or --port <UDP port>           : base port from which to iterate sessions port "
		"IDs\n");
	printf("   --rx                                         : run receive mode only \n");
	printf("   --tx                                         : run transmit mode only \n");
	printf("   --rgba                                       : input / output buffers are in rgba format\n");
	printf("   --yuv10be                                    : input / output buffers are in yuv10be format\n");
	printf("   --format <fmt string>                        : select frame format e.g. a1080i50 = all 1080 interlaced 50fps\n");
	printf("                                                    : e.g. i720p29  = intel 720 progressive 29.97fps\n");
	printf("                                                    : e.g. i1080p59 = intel 1080 progressive 59.94fps\n");
	printf("                                                    : e.g. i2160p59 = intel 2160 progressive 59.94fps\n");
	printf("                                                    : e.g. i1080i29 = intel 1080 interlaced 29.97fps\n");
	printf("                                                    : e.g. a1080p59 = all 1080 progressive 59.94fps\n");
	printf("   --s_count <number of sessions>               : number of sessions \n");
	printf("   --o_port <PCI device address>                : output interface PCI device address \n");
	printf("   --in_port <PCI device address>               : input interface PCI device address \n");
	printf("   --ptpid <hhhhhh.hhhh.hhhhhh>                 : master clock id - it will be used in ptp - disable BKC choosing algorithm\n");
	printf("   --ptpam <u|m>                                : type of addresing for request in ptp\n");
	printf("                                                    : m - multicast (default)\n");
    printf("                                                    : u - unicast\n");
	printf("   --ptpstp <o|t>                               : use one step ort two for ptp - default two\n");
	printf("                                                    : o - one step - not supportet yet\n");
    printf("                                                    : t - two step (default)\n");
	printf("   --log_level <user,level<info/debug/error>>   : enable additional logs \n");
	printf("\n");
}

#define MAKE_DWORD_FROM_CHAR(hh,hl,lh,ll) ((hh << 24) | (hl << 16) | (lh << 8) | (ll)
#define MAKE_WORD_FROM_CHAR(h,l) ((h << 8) | (l))

int
ParseArgs(int argc, char *argv[], st_user_params_t *outParams)
{
	int c;
	int nargs = 0;
	st_param_val_t stParamVal;

	ShowWelcomeBanner();

	outParams->rate = 29;
	outParams->fmtIndex = 1;
	outParams->udpPort = 10000;
	outParams->snCount = MAX_SESSIONS_MAX;
	outParams->bufFormat = ST21_BUF_FMT_YUV_422_10BIT_BE;

	stParamVal.valueU64 = outParams->snCount;
	StSetParam(ST_SN_COUNT, stParamVal);

	char isIntel = 'a';
	int32_t height = 0;

	while (1)
	{
		int optIdx = 0;
		static struct option options[] = { { "ip", required_argument, 0, 1 },
										   { "sip", required_argument, 0, 2 },
										   { "rx", no_argument, 0, 3 },
										   { "tx", no_argument, 0, 4 },
										   { "rgba", no_argument, 0, 5 },
										   { "yuv10be", no_argument, 0, 6 },
										   { "ebu", no_argument, 0, 'e' },
										   { "log_level", required_argument, 0, 'l' },
										   { "s_count", required_argument, 0, 's' },
										   { "ptpid", required_argument, 0, MAKE_WORD_FROM_CHAR('p','i') },
										   { "ptpam", required_argument, 0, MAKE_WORD_FROM_CHAR('p','m')  },
										   { "ptpstp", required_argument, 0, MAKE_WORD_FROM_CHAR('p','s')  },
										   { "mac", required_argument, 0, 'm' },
										   { "o_port", required_argument, 0, 'o' },
										   { "in_port", required_argument, 0, 'i' },
										   { "format", required_argument, 0, 'f' },
										   { "port", required_argument, 0, 'p' },
										   { "help", no_argument, 0, 'h' },
										   { "version", no_argument, 0, 'v' },
										   { 0, 0, 0, 0 } };

		c = getopt_long_only(argc, argv, "hv", options, &optIdx);

		if (c == -1)
			break;

		switch (c)
		{
		case 1:
			if (inet_pton(AF_INET, optarg, outParams->ipAddr) != 1)
			{
				rte_exit(127, "%s is not IP\n", optarg);
			}
			memcpy((uint8_t *)&stParamVal.valueU32, outParams->ipAddr, 4);
			StSetParam(ST_DESTINATION_IP, stParamVal);
			break;

		case 2:
			if (inet_pton(AF_INET, optarg, outParams->sipAddr) != 1)
			{
				rte_exit(127, "%s is not IP\n", optarg);
			}
			memcpy((uint8_t *)&stParamVal.valueU32, outParams->sipAddr, 4);
			StSetParam(ST_SOURCE_IP, stParamVal);
			break;

		case 3:
			outParams->rxOnly = 1;
			stParamVal.valueU64 = 1;
			StSetParam(ST_RX_ONLY, stParamVal);
			break;

		case 4:
			outParams->txOnly = 1;
			stParamVal.valueU64 = 1;
			StSetParam(ST_TX_ONLY, stParamVal);
			break;

		case 5:
			outParams->bufFormat = ST21_BUF_FMT_RGBA_8BIT;
			break;

		case 6:
			outParams->bufFormat = ST21_BUF_FMT_YUV_422_10BIT_BE;
			break;

		case 'e':
			outParams->isEbuCheck = true;
			stParamVal.valueU64 = outParams->isEbuCheck;
			StSetParam(ST_EBU_TEST, stParamVal);
			break;
		case 'l':
			stParamVal.strPtr = optarg;
			StSetParam(ST_DPDK_PARAMS, stParamVal);
			break;
		case 's':
			outParams->snCount = atoi(optarg);
			stParamVal.valueU64 = outParams->snCount;
			StSetParam(ST_SN_COUNT, stParamVal);
			break;

		case MAKE_WORD_FROM_CHAR('p','i'):  // ptp clock id
		{
			st_ptp_clock_id_t clockId;
			if (sscanf(optarg, "%2hhx%2hhx%2hhx.%2hhx%2hhx.%2hhx%2hhx%2hhx", clockId.id + 0,
				clockId.id + 1, clockId.id + 2, clockId.id + 3, clockId.id + 4,
				clockId.id + 5, clockId.id + 6, clockId.id + 7)
				== 8)
			{
				stParamVal.ptr = (void*)&clockId;
				StPtpSetParam(ST_PTP_CLOCK_ID,stParamVal);
				stParamVal.valueU32 = ST_PTP_SET_MASTER;
				StPtpSetParam(ST_PTP_CHOOSE_CLOCK_MODE,stParamVal);
			}
			break;
		}
		case MAKE_WORD_FROM_CHAR('p','m'):
        {
            char m = optarg[0];
            if (m != 'm' && m != 'u') break;
            stParamVal.valueU32 = m == 'm'? ST_PTP_MULTICAST_ADDR: ST_PTP_UNICAST_ADDR;
            StPtpSetParam(ST_PTP_ADDR_MODE, stParamVal);
            break;
        }
		case MAKE_WORD_FROM_CHAR('p','s'):
        {
            char s = optarg[0];
            if (s != 'o' && s!= 't') break;
            stParamVal.valueU32 = s == 't'? ST_PTP_TWO_STEP: ST_PTP_ONE_STEP;
            StPtpSetParam(ST_PTP_STEP_MODE, stParamVal);
            break;
        }
		case 'm':
			if (sscanf(optarg, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &outParams->macAddr[0],
				&outParams->macAddr[1], &outParams->macAddr[2], &outParams->macAddr[3],
				&outParams->macAddr[4], &outParams->macAddr[5])
				== 6)
			{
				memcpy(&stParamVal.valueU64, outParams->macAddr, 6);
				StSetParam(ST_MAC, stParamVal);
			}
			break;

		case 'o':
			outParams->outPortName = optarg;
			stParamVal.strPtr = outParams->outPortName;
			StSetParam(ST_OUT_PORT, stParamVal);
			break;

		case 'i':
			outParams->inPortName = optarg;
			stParamVal.strPtr = outParams->inPortName;
			StSetParam(ST_IN_PORT, stParamVal);
			break;

		case 'f':
		{
			char interlaced;
			if (sscanf(optarg, "%1c%u%1c%u", &isIntel, &height, &interlaced, &outParams->rate) == 4)
			{
				switch (isIntel)
				{
				default:
					rte_exit(127, "Invalid prefix used, allowed: a, i\n");
				case 'a':
				case 'i':;
				}
				switch (height)
				{
				default:
					rte_exit(127, "Invalid frame heigth used, allowed: 720, 1080, 2160\n");
				case 720:
					outParams->fmtIndex = (isIntel == 'i') ? 0 : 3;
					break;
				case 1080:
					outParams->fmtIndex = (isIntel == 'i') ? 1 : 4;
					break;
				case 2160:
					outParams->fmtIndex = (isIntel == 'i') ? 2 : 5;
					break;
				}
				switch (interlaced) {
				default:
					rte_exit(127, "Invalid interlaced used, allowed: i, p\n");
				case 'i':
					outParams->interlaced = 1;
					break;
				case 'p':
					outParams->interlaced = 0;
					break;
				}
				switch (outParams->rate)
				{
				default:
					rte_exit(127, "Invalid rate, allowed: 25, 29,50, 59\n");
				case 25:
				case 29:
				case 50:
				case 59:;
				}
			}
			else
			{
				rte_exit(127, "Invalid format, example: a1080p29\n");
			}
			stParamVal.valueU64 = outParams->fmtIndex;
			StSetParam(ST_FMT_INDEX, stParamVal);
		}
		break;

		case 'p':
			outParams->udpPort = atoi(optarg);
			break;

		case 'h':
			PrintHelp();
			rte_exit(0, " ");

		case 'v':
			PrintVersion();
			rte_exit(0, " ");

		case '?':
			break;
		case 0:
			rte_exit(0, "Invalid arguments provided!\n");
		default:
			PrintHelp();
			rte_exit(0, " ");
		}
		nargs = optind;
	}
	/* Verify if args were consistent
	 */
	if (outParams->fmtIndex >= ST21_FMT_MAX)
	{
		PrintHelp();
		rte_exit(ST_FMT_ERR_BAD_HEIGHT, "Invalid Format ID used");
	}
	RTE_LOG(INFO, USER1, "Chosen FMT is %s%d%s%d\n", (isIntel == 'i') ? "intel " : "all ", height,
		(outParams->interlaced) ? "i" : "p", outParams->rate);

	if (nargs == argc)
	{
		rte_exit(ST_GENERAL_ERR, "Application exited because of wrong usage\n");
	}
	return nargs;
}

st_status_t
InitFormat(st_user_params_t userParams, st21_format_t **txFmtOut, st21_format_t **rxFmtOut,
		   st_device_t *confTx, st_device_t *confRx)
{
	st21_format_t *txFmt = NULL;
	st21_format_t *rxFmt = NULL;

	/// First initialization of format params
	if (userParams.fmtIndex == 0 || userParams.fmtIndex == 3)
	{
		txFmt = &sln422be10Hd720p29Fmt;
		rxFmt = &sln422be10Hd720p29Fmt;
	}
	else if (userParams.fmtIndex == 1 || userParams.fmtIndex == 4)
	{
		txFmt = &sln422be10Hd1080p29Fmt;
		rxFmt = &sln422be10Hd1080p29Fmt;
	}

	/// Proper initialization of the format params (sensitiwe for all related input params)
	if (userParams.interlaced)
	{
		switch (userParams.rate)
		{
		case 25:
			confRx->exactRate = ST_DEV_RATE_I_25_00;
			confTx->exactRate = ST_DEV_RATE_I_25_00;
			txFmt = fmtI25Table[userParams.fmtIndex % ST21_FMT_TX_MAX];
			rxFmt = fmtI25Table[userParams.fmtIndex];
			break;
		case 29:
			confRx->exactRate = ST_DEV_RATE_I_29_97;
			confTx->exactRate = ST_DEV_RATE_I_29_97;
			txFmt = fmtI29Table[userParams.fmtIndex % ST21_FMT_TX_MAX];
			rxFmt = fmtI29Table[userParams.fmtIndex];
			break;
		case 50:
			confRx->exactRate = ST_DEV_RATE_I_50_00;
			confTx->exactRate = ST_DEV_RATE_I_50_00;
			txFmt = fmtI50Table[userParams.fmtIndex % ST21_FMT_TX_MAX];
			rxFmt = fmtI50Table[userParams.fmtIndex];
			break;
		case 59:
			confRx->exactRate = ST_DEV_RATE_I_59_94;
			confTx->exactRate = ST_DEV_RATE_I_59_94;
			txFmt = fmtI59Table[userParams.fmtIndex % ST21_FMT_TX_MAX];
			rxFmt = fmtI59Table[userParams.fmtIndex];
			break;
		default:
			ST_APP_ASSERT;
			break;
		}
	}
	else
	{
		switch (userParams.rate)
		{
		case 25:
			confRx->exactRate = ST_DEV_RATE_P_25_00;
			confTx->exactRate = ST_DEV_RATE_P_25_00;
			txFmt = fmtP25Table[userParams.fmtIndex % ST21_FMT_TX_MAX];
			rxFmt = fmtP25Table[userParams.fmtIndex];
			break;
		case 29:
			confRx->exactRate = ST_DEV_RATE_P_29_97;
			confTx->exactRate = ST_DEV_RATE_P_29_97;
			txFmt = fmtP29Table[userParams.fmtIndex % ST21_FMT_TX_MAX];
			rxFmt = fmtP29Table[userParams.fmtIndex];
			break;
		case 50:
			confRx->exactRate = ST_DEV_RATE_P_50_00;
			confTx->exactRate = ST_DEV_RATE_P_50_00;
			txFmt = fmtP50Table[userParams.fmtIndex % ST21_FMT_TX_MAX];
			rxFmt = fmtP50Table[userParams.fmtIndex];
			break;
		case 59:
			confRx->exactRate = ST_DEV_RATE_P_59_94;
			confTx->exactRate = ST_DEV_RATE_P_59_94;
			txFmt = fmtP59Table[userParams.fmtIndex % ST21_FMT_TX_MAX];
			rxFmt = fmtP59Table[userParams.fmtIndex];
			break;
		default:
			ST_APP_ASSERT;
			break;
		}
	}

	*txFmtOut = txFmt;
	*rxFmtOut = rxFmt;

	return ST_OK;
}

st_status_t
InitTransmitter(st_user_params_t userParams, st_device_t **txDevOut, st_device_t confTx)
{
	st_status_t stat = ST_OK;

	st_device_t *txDev = NULL;

	/// Create TX device
	stat = StCreateDevice(&confTx, userParams.outPortName, userParams.outPortName, &txDev);
	if (stat != ST_OK)
	{
		RTE_LOG(ERR, USER1, "StCreateDevice TX FAILED. ErrNo: %d\n", stat);
		return stat;
	}
	RTE_LOG(INFO, USER1, "Create TX device done\n");

	/// Return handle to the created device
	*txDevOut = txDev;

	return stat;
}

st_status_t
InitReceiver(st_user_params_t userParams, st_device_t **rxDevOut, st_device_t confRx)
{
	st_status_t stat = ST_OK;

	st_device_t *rxDev = NULL;

	/// Create RX device
	stat = StCreateDevice(&confRx, userParams.inPortName, userParams.inPortName, &rxDev);
	if (stat != ST_OK)
	{
		RTE_LOG(ERR, USER1, "StCreateDevice RX FAILED. ErrNo: %d\n", stat);
		return stat;
	}
	RTE_LOG(INFO, USER1, "Create RX device done\n");

	/// Return handle to the created device
	*rxDevOut = rxDev;

	return stat;
}

st_status_t
StartTransmitter(st_user_params_t userParams, st21_format_t *txFmt, st21_session_t **txSnOut,
				 st_device_t *txDev)
{
	st_status_t stat = ST_OK;

	st21_session_t *txSn[txDev->snCount];
	int isSendView = 0;

	/// Loop for create sessions
	for (uint32_t i = 0; (i < txDev->snCount) && (userParams.rxOnly == 0); i++)
	{
		/// Input parameters used by \ref St21CreateSession
		st21_session_t txSnIn = { 0 };
		st_addr_t txAddr;
		txSn[i] = NULL;
		rvrtp_send_app_t *txApp = NULL;

		txSnIn.nicPort[0] = txDev->port[0];
		txSnIn.nicPort[1] = txDev->port[1];
		txSnIn.caps = ST21_SN_SINGLE_PATH | ST21_SN_UNICAST | ST21_SN_CONNECTLESS;
		txSnIn.ssid = 0x123450 + i;

		/// Create session with given parameters
		stat = St21CreateSession(txDev, &txSnIn, txFmt, &txSn[i]);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "St21CreateSession FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Input parameters used by \ref St21BindIpAddr
		memset(&txAddr, 0, sizeof(txAddr));
		txAddr.src.addr4.sin_family = AF_INET;
		txAddr.src.addr4.sin_port = htons(userParams.udpPort + txSn[i]->timeslot);
		txAddr.dst.addr4.sin_port = htons(userParams.udpPort + txSn[i]->timeslot);
		memcpy((uint8_t *)&txAddr.src.addr4.sin_addr.s_addr, &userParams.sipAddr, 4);
		memcpy((uint8_t *)&txAddr.dst.addr4.sin_addr.s_addr, &userParams.ipAddr, 4);

		/// Bind IP addresses with proper MAC and fill addresses in the flow table
		stat = St21BindIpAddr(txSn[i], &txAddr, 0);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "St21BindIpAddr FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Get content prepare send mechanism and \ref St21RegisterProducer
		stat = SendAppCreateProducer(txSn[i], userParams.bufFormat, userParams.fmtIndex, &txApp);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "SendAppCreateProducer FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Create viewer to enable presenting transmitted content on the screen
		txApp->view = NULL;
		if (isSendView == 0)
		{
			char label[256];
			st21_format_t fmt;
			snprintf(label, sizeof(label), "SENDER: ");
			St21GetFormat(txSn[i], &fmt);
			stat = CreateView(&txApp->view, label, userParams.bufFormat, fmt.width, fmt.height);
			if (stat != ST_OK)
			{
				RTE_LOG(ERR, USER1, "CreateView sender FAILED. ErrNo: %d\n", stat);
				return stat;
			}
			ShowFrame(txApp->view, txApp->movie, 2);
			isSendView = 1;
		}

		/// Set transmitter ready for sending by call \ref St21ProducerStartFrame
		stat = SendAppStart(txSn[i], txApp);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "SendAppStart FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Return handle to the created session
		txSnOut[i] = txSn[i];
	}

	/// Run threads for generating frames and for sending them
	stat = StStartDevice(txDev);
	if (stat != ST_OK)
	{
		RTE_LOG(ERR, USER1, "StStartDevice (TX) FAILED. ErrNo: %d\n", stat);
		return stat;
	}

	return stat;
}

st_status_t
StartReceiver(st_user_params_t userParams, st21_format_t *rxFmt, st21_session_t **rxSnOut,
			  st_device_t *rxDev, rvrtp_recv_app_t **rxAppOut)
{
	st_status_t stat = ST_OK;

	st21_session_t *rxSn[rxDev->snCount];
	rvrtp_recv_app_t *rxApp[rxDev->snCount];

	/// Loop for create sessions
	for (uint32_t i = 0; (i < rxDev->snCount) && (userParams.txOnly == 0); i++)
	{
		/// Input parameters used by \ref St21CreateSession
		st21_session_t rxSnIn = { 0 };
		st_addr_t rxAddr;
		rxSn[i] = NULL;
		rxApp[i] = NULL;

		rxSnIn.nicPort[0] = rxDev->port[0];
		rxSnIn.nicPort[1] = rxDev->port[1];
		rxSnIn.caps = ST21_SN_SINGLE_PATH | ST21_SN_UNICAST | ST21_SN_CONNECTLESS;
		rxSnIn.ssid = 0x123450 + i;
		rxSnIn.timeslot = i;

		/// Create session with given parameters
		stat = St21CreateSession(rxDev, &rxSnIn, rxFmt, &rxSn[i]);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "St21CreateSession FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Input parameters used by \ref St21BindIpAddr
		memset(&rxAddr, 0, sizeof(rxAddr));
		rxAddr.src.addr4.sin_family = AF_INET;
		rxAddr.src.addr4.sin_port = htons(userParams.udpPort + rxSn[i]->timeslot);
		rxAddr.dst.addr4.sin_port = htons(userParams.udpPort + rxSn[i]->timeslot);
		memcpy((uint8_t *)&rxAddr.src.addr4.sin_addr.s_addr, &userParams.sipAddr, 4);
		memcpy((uint8_t *)&rxAddr.dst.addr4.sin_addr.s_addr, &userParams.ipAddr, 4);

		/// Bind IP addresses with proper MAC and fill addresses in the flow table
		stat = St21BindIpAddr(rxSn[i], &rxAddr, 0);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "St21BindIpAddr FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		/// Prepare receive mechanism and \ref St21RegisterConsumer
		stat = RecvAppCreateConsumer(rxSn[i], userParams.bufFormat, &rxApp[i]);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "RecvAppCreateConsumer FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Create viewer to enable presenting transmitted content on the screen
		char label[256];
		st21_format_t fmt;
		snprintf(label, sizeof(label), "RECEIVER: ");
		St21GetFormat(rxSn[i], &fmt);
		stat = CreateView(&rxApp[i]->view, label, userParams.bufFormat, fmt.width, fmt.height);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "CreateView FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Set receiver ready for receive by call \ref St21ConsumerStartFrame
		stat = RecvAppStart(rxSn[i], rxApp[i]);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "RecvAppStart FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		rxSnOut[i] = rxSn[i];
		rxAppOut[i] = rxApp[i];
	}

	/// Run threads for receiving frames
	stat = StStartDevice(rxDev);
	if (stat != ST_OK)
	{
		RTE_LOG(ERR, USER1, "StStartDevice (RX) FAILED. ErrNo: %d\n", stat);
		return stat;
	}

	return stat;
}

st_status_t
FinishTransmitter(st21_session_t **txSnOut, uint32_t snTxCount)
{
	st_status_t stat = ST_OK;

	// Destroy TX session
	for (uint32_t i = 0; i < snTxCount; i++)
	{
		/// Finish sending frames
		stat = St21ProducerStop(txSnOut[i]);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "St21ProducerStop FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		/// Destroy transmitter session
		stat = St21DestroySession(txSnOut[i]);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "St21DestroySession FAILED. ErrNo: %d\n", stat);
			return stat;
		}
	}
	RTE_LOG(INFO, USER1, "Producer STOPPED and destroyed\n");
	// Destroy TX session end

	return stat;
}

st_status_t
FinishReceiver(st21_session_t **rxSnOut, uint32_t snRxCount, rvrtp_recv_app_t **app)
{
	st_status_t stat = ST_OK;

	// Destroy RX session
	for (uint32_t i = 0; i < snRxCount; i++)
	{
		/// Finish receiving frames
		stat = RecvAppStop(rxSnOut[i], app[i]);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "St21ConsumerStop FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		/// Destroy receiver session
		stat = St21DestroySession(rxSnOut[i]);
		if (stat != ST_OK)
		{
			RTE_LOG(ERR, USER1, "St21DestroySession FAILED. ErrNo: %d\n", stat);
			return stat;
		}
	}
	RTE_LOG(INFO, USER1, "Consumer STOPPED and destroyed\n");
	// Destroy RX session end

	return stat;
}

/** Main point of the entire solution
* This is place where program is started
*/
int
main(int argc, char *argv[])
{
	st_status_t status = ST_OK;

	/// Initialization of variables
	st_user_params_t userParams = { 0 };
	uint32_t snTxCount = 0;
	uint32_t snRxCount = 0;
	st21_format_t *txFmt = NULL;
	st21_format_t *rxFmt = NULL;
	st_device_t *txDev = NULL;
	st_device_t *rxDev = NULL;

	/// STEP 1 - Preparing configuration for device initialization
	st_device_t confRx = {
		.type = ST_DEV_TYPE_CONSUMER,
		.exactRate = ST_DEV_RATE_P_29_97,
	};
	st_device_t confTx = {
		.type = ST_DEV_TYPE_PRODUCER,
		.exactRate = ST_DEV_RATE_P_29_97,
	};

	/** STEP 2 - Parsing command line arguments
	* This function is responsible for parsing arguments from command line,
	* fill and return struct with parameters (userParams).
	* Params:
	*	@parama rgc  - number of arguments in command line
	*	@param argv  - table with arguments from command line
	*	@param userParams - structure of parameters (type: st_user_params_t) filled and returned after parse args
	*
	* @return number of arguments
	*/
	ParseArgs(argc, argv, &userParams);

	/** STEP 3 - Select proper format for transmit and receive.
	* Format is related with image parameters such as width and heigh of image, pixel format etc.
	* Params:
	*	@param userParams - structure of parameters (type: \ref st_user_params_t) filled and returned after parse args
	*	@param txFmt - container prepared for media related data, intended for transmitter
	*	@param rxFmt - container prepared for media related data, intended for receiver
	*	@param confTx - structure with basic information about transmitter needed for proper initialize format parameters
	*			 for media session
	*	@param confRx - structure with basic information about receiver needed for proper initialize format parameters
	*			 for media session
	*
	* @return \ref st_status_t
	*/
	status = InitFormat(userParams, &txFmt, &rxFmt, &confTx, &confRx);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "FormatInit FAILED. ErrNo: %d\n", status);
		return status;
	}

	/** STEP 4 - Create and initialize of transmitter device
	* Params:
	*	@param userParams - structure with input parameters
	*	@param txDev - container prepared for the device created inside the function
	*	@param confTx - structure with basic information about transmitter needed for proper initialize device
	*
	* @return \ref st_status_t
	*/
	status = InitTransmitter(userParams, &txDev, confTx);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "InitTransmitter FAILED. ErrNo: %d\n", status);
		return status;
	}

	/** STEP 5 - Create and initialize of receiver device
	* Params:
	*	@param userParams - structure with input parameters
	*	@param rxDev - container prepared for the device created inside the function
	*	@param confRx - structure with basic information about receiver needed for proper initialize device
	*
	* @return \ref st_status_t
	*/
	status = InitReceiver(userParams, &rxDev, confRx);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "InitReceiver FAILED. ErrNo: %d\n", status);
		return status;
	}

	st21_session_t *rxSn[rxDev->snCount];
	st21_session_t *txSn[txDev->snCount];
	rvrtp_recv_app_t *rxApp[rxDev->snCount];

	/** STEP 6 - Initialization of Silmple DirectMedia Library
	* Library used for presenting transmitted content on the screen
	*
	* @return \ref st_status_t
	*/
	status = InitSDL();
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "InitSDL FAILED. ErrNo: %d\n", status);
	}

	/** STEP 7 - Create and initialize transmitter sessions
	* Function responsible for create proper number of sessions, initialize network parameters and start transmission
	* Params:
	*	@param userParams - structure with input parameters
	*	@param txFmt - container prepared for media related data, intended for transmitter
	*	@param txSn - table prepared for storage created transmitter sessions
	*	@param txDev - transmitter device (created sessions will use this device for transmitting)
	*
	* @return \ref st_status_t
	*/
	status = StartTransmitter(userParams, txFmt, txSn, txDev);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "StartReceiver FAILED. ErrNo: %d\n", status);
		return status;
	}

	/** STEP 8 - Create and initialize receiver sessions
	* Function responsible for create proper number of sessions, initialize network parameters and start receiving
	* Params:
	*	@param userParams - structure with input parameters
	*	@param rxFmt - container prepared for media related data, intended for receiver
	*	@param rxSn - table prepared for storage created receiver sessions
	*	@param rxDev - receiver device (created sessions will use this device for receiving)
	*
	* @return \ref st_status_t
	*/
	status = StartReceiver(userParams, rxFmt, rxSn, rxDev, rxApp);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "StartReceiver FAILED. ErrNo: %d\n", status);
		return status;
	}

	/** STEP 9 - API function responsible for get actual number of created transmitter sessions
	* Params:
	*	@param txDev - transmitter device (returned number of sessions will be number for this device)
	*	@param snTxCount - container for returned number of created sessions
	*
	* @return \ref st_status_t
	*/
	status = St21GetSessionCount(txDev, &snTxCount);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "St21GetSessionCount FAILED. ErrNo: %d\n", status);
		return status;
	}

	RTE_LOG(INFO, USER1, "Create TX sessions done. Number of sessions: %u\n", snTxCount);

	/** STEP 10 - API function responsible for get actual number of created receiver sessions
	* Params:
	*	@param rxDev - receiver device (returned number of sessions will be number for this device)
	*	@param snRxCount - container for returned number of created sessions
	*
	* @return \ref st_status_t
	*/
	status = St21GetSessionCount(rxDev, &snRxCount);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "St21GetSessionCount FAILED. ErrNo: %d\n", status);
		return status;
	}

	RTE_LOG(INFO, USER1, "Create RX sessions done. Number of sessions: %u\n", snRxCount);

	/** STEP 11 - Wait until signal caught
	* "pause" prevent before immediate finish and close transmission
	*/
	pause();

	/** STEP 12 - Stop transmitting and destroy transmitter sessions
	* Params:
	*	@param txSn - table of created transmitter sessions which will be closed and destroyed
	*	@param snTxCount - number of transmitter sessions
	*
	* @return \ref st_status_t
	*/
	status = FinishTransmitter(txSn, snTxCount);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "FinishTransmitter FAILED. ErrNo: %d\n", status);
		return status;
	}

	/** STEP 13 - Stop receiving and destroy receiver sessions
	* Params:
	*	@param rxSn - table of created receiver sessions which will be closed and destroyed
	*	@param snRxCount - number of receiver sessions
	*
	* @return \ref st_status_t
	*/
	status = FinishReceiver(rxSn, snRxCount, rxApp);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "FinishReceiver FAILED. ErrNo: %d\n", status);
		return status;
	}

	/** STEP 14 - Destroy transmitter device
	* Params:
	*	@param rxSn - rxDev - transmitter device
	*
	* @return \ref st_status_t
	*/
	status = StDestroyDevice(txDev);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "StDestroyDevice FAILED. ErrNo: %d\n", status);
		return status;
	}

	/** STEP 15 -Destroy receiver device
	* Params:
	*	@param rxDev - receiver device
	*
	* @return \ref st_status_t
	*/
	status = StDestroyDevice(rxDev);
	if (status != ST_OK)
	{
		RTE_LOG(ERR, USER1, "StDestroyDevice FAILED. ErrNo: %d\n", status);
		return status;
	}

	return status;
}

