/*
* Copyright (C) 2020-2021 Intel Corporation.
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

#ifndef ST_MAX_AUDIO_PKT_SIZE
#define ST_MAX_AUDIO_PKT_SIZE 1200
#endif

typedef struct st_user_params
{
	/* Input Parameters */
	int pTx;
	int pRx;
	int rTx;
	int rRx;
	int numPorts;
	bool isEbuCheck;
	uint8_t ipAddr[MAX_RXTX_PORTS][MAX_RXTX_TYPES][4];	/**< destination IP for TX and RX*/
	uint8_t sipAddr[MAX_RXTX_PORTS][4]; /**< source IP */
	uint32_t rate;
	uint32_t interlaced;
	uint32_t fmtIndex;
	uint16_t udpPort;
	uint16_t appSCoreId;
	uint32_t snCount;
	uint32_t sn30Count;
	uint32_t sn40Count;
	st21_buf_fmt_t bufFormat;
	uint32_t showframeInTx; /* whether show frame in tx, default is off */
	uint16_t audioFrameSize;

	char *pPortName;
	char *rPortName;

	char *videoFileName;
	char *audioFileName;
	char *anciliaryFileName;

} st_user_params_t;

rxtxapp_main_t rxtx_main;

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
	printf("API version: %d.%d.%d\n", ST_VERSION_MAJOR, ST_VERSION_MINOR,
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
	printf("   --p_tx_ip <IP addr>                          : destination TX IP address for primary port(required when p_tx = 1) \n");
	printf("   --r_tx_ip <IP addr>                          : destination TX IP address for redundant port(required when r_tx = 1) \n");
	printf("   --p_rx_ip <IP addr>                          : destination RX IP address for primary port(required when p_rx = 1) \n");
	printf("   --r_rx_ip <IP addr>                          : destination RX IP address for redundant port(required  when r_rx = 1) \n");
	printf("   --sip <IP addr>                              : user defined source IP address, if "
		   "not set, get it from kernel\n");
	printf("   --rsip <IP addr>                             : user defined source redundant IP "
		   "address, if not set, get it from kernel\n");
	printf("   --ebu                                        : enable EBU compatibility with "
		   "standard ST 2110 "
		   "logs\n");
	printf("   -p <UDP port> or --port <UDP port>           : base port from which to iterate "
		   "sessions port "
		   "IDs\n");
	printf("   --p_tx                                       : run transmit from primary port (required)\n");
	printf("   --p_rx                                       : run receive from primary port \n");
	printf("   --r_tx                                       : run transmit from redundant port \n");
	printf("   --r_rx                                       : run receive from redundant port \n");
	printf("   --display                                    : display video for tx, default is "
		   "off(on will impact tx performance) \n");
	printf("   --format <fmt string>                        : select frame format e.g. a1080i50 = "
		   "all 1080 interlaced 50fps\n");
	printf("                                                    : e.g. i720p29  = intel 720 "
		   "progressive 29.97fps\n");
	printf("                                                    : e.g. i1080p59 = intel 1080 "
		   "progressive 59.94fps\n");
	printf("                                                    : e.g. i2160p59 = intel 2160 "
		   "progressive 59.94fps\n");
	printf("                                                    : e.g. i1080i29 = intel 1080 "
		   "interlaced 29.97fps\n");
	printf("                                                    : e.g. a1080p59 = all 1080 "
		   "progressive 59.94fps\n");
	printf("   --s_count <number of sessions>               : number of ST2110-20 (Video) sessions "
		   "\n");
	printf("   --s30_count <number of sessions>               : number of ST2110-30 (audio) "
		   "sessions \n");
	printf("   --s40_count <number of sessions>               : number of ST2110-40 (ancillary) "
		   "sessions \n");
	printf("   --app_scid <core id>                         : application start core id \n");
	printf("   --lib_cid <cores id>                         : library core id e.g. 1,2,3,4 \n");
	printf("   --p_port <PCI device address>                : primary interface PCI device address "
		   "\n");
	printf("   --r_port <PCI device address>                : redundant interface PCI device "
		   "address \n");
	printf("   --ptpid <hhhhhh.hhhh.hhhhhh>                 : master clock id - it will be used in "
		   "ptp - disable BKC choosing algorithm\n");
	printf(
		"   --ptpam <u|m>                                : type of addresing for request in ptp\n");
	printf("                                                    : m - multicast (default)\n");
	printf("                                                    : u - unicast\n");
	printf("   --ptpstp <o|t>                               : use one step ort two for ptp - "
		   "default two\n");
	printf(
		"                                                    : o - one step - not supportet yet\n");
	printf("                                                    : t - two step (default)\n");
	printf("   --log_level <user,level<info/debug/error>>   : enable additional logs \n");
	printf("   --videoFile  <filename>                      : specyfying the path to send video "
		   "file \n");
	printf("   --audioFile  <filename>                      : specyfying the path to send audio "
		   "file \n");
	printf("   --ancFile  <filename>                        : specyfying the path to send "
		   "amciliary file \n");
	printf("   --audioFrame  <Audio frame size>             : Size of Audio frame in bytes, user "
		   "provides based on frequency, channel count and bit depth for desired duration of audio "
		   "samples (e.g. 1ms) \n");
	printf("   --pacing <control way>			: select pacing type e.g. pause, ratelimit or tsc\n");
	printf("   --tsc_hz <hz>			        : User specified tsc frequency\n");
	printf("   --user_timestamp                 : User provide timestamp values for RTP header via ST_API calls\n");
	printf("\n");
}

#define MAKE_DWORD_FROM_CHAR(hh, hl, lh, ll) ((hh << 24) | (hl << 16) | (lh << 8) | (ll)
#define MAKE_WORD_FROM_CHAR(h, l) ((h << 8) | (l))

int
ParseArgs(int argc, char *argv[], st_user_params_t *outParams)
{
	int c;
	int nargs = 0;
	st_param_val_t stParamVal;

	ShowWelcomeBanner();

	stParamVal.valueU64 = outParams->snCount;
	StSetParam(ST_SN_COUNT, stParamVal);
	stParamVal.valueU64 = outParams->sn30Count;
	StSetParam(ST_SN30_COUNT, stParamVal);
	stParamVal.valueU64 = outParams->sn40Count;
	StSetParam(ST_SN40_COUNT, stParamVal);
	stParamVal.strPtr = '\0';
	StSetParam(ST_P_PORT, stParamVal);
	StSetParam(ST_R_PORT, stParamVal);

	char isIntel = 'a';
	int32_t height = 0;

	char *token = '\0';
   	char *ptr = NULL;
	char *exitStr = '\0';
	int strLengh = 0;
   	long int lib_core = 0;

	while (1)
	{
		int optIdx = 0;
		static struct option options[]
			= { { "p_tx_ip", required_argument, 0, 1 },
				{ "sip", required_argument, 0, 2 },
				{ "p_tx", no_argument, 0, 3 },
				{ "p_rx", no_argument, 0, 4 },
				{ "r_tx", no_argument, 0, 5 },
				{ "r_rx", no_argument, 0, 6 },
				{ "r_tx_ip", required_argument, 0, 7 },
				{ "rsip", required_argument, 0, 8 },
				{ "display", no_argument, 0, 9 },
				{ "p_rx_ip", required_argument, 0, 10 },
				{ "r_rx_ip", required_argument, 0, 11 },
				{ "ebu", no_argument, 0, 'e' },
				{ "log_level", required_argument, 0, 'l' },
				{ "s_count", required_argument, 0, 's' },
				{ "s30_count", required_argument, 0, MAKE_WORD_FROM_CHAR('s', '3') },
				{ "s40_count", required_argument, 0, MAKE_WORD_FROM_CHAR('s', '4') },
				{ "app_scid", required_argument, 0, 'c' },
				{ "lib_cid", required_argument, 0, MAKE_WORD_FROM_CHAR('c', 'l') },
				{ "ptpid", required_argument, 0, MAKE_WORD_FROM_CHAR('p', 'i') },
				{ "ptpam", required_argument, 0, MAKE_WORD_FROM_CHAR('p', 'm') },
				{ "ptpstp", required_argument, 0, MAKE_WORD_FROM_CHAR('p', 's') },
				{ "mac", required_argument, 0, 'm' },
				{ "p_port", required_argument, 0, 'o' },
				{ "r_port", required_argument, 0, 'i' },
				{ "audio", required_argument, 0, 'a' },
				{ "format", required_argument, 0, 'f' },
				{ "port", required_argument, 0, 'p' },
				{ "videoFile", required_argument, 0, MAKE_WORD_FROM_CHAR('v', 'f') },
				{ "audioFile", required_argument, 0, MAKE_WORD_FROM_CHAR('a', 'f') },
				{ "ancFile", required_argument, 0, MAKE_WORD_FROM_CHAR('c', 'f') },
				{ "bulk_num", required_argument, 0, MAKE_WORD_FROM_CHAR('b', 'n') },
				{ "enqueue_threads", required_argument, 0, MAKE_WORD_FROM_CHAR('e', 't') },
				{ "audioFrame", required_argument, 0, MAKE_WORD_FROM_CHAR('a', 's') },
				{ "pacing", required_argument, 0, MAKE_WORD_FROM_CHAR('p', 'c') },
				{ "tsc_hz", required_argument, 0, MAKE_WORD_FROM_CHAR('t', 'h') },
				{ "rl_Bps", required_argument, 0, MAKE_WORD_FROM_CHAR('r', 'B') },
				{ "help", no_argument, 0, 'h' },
				{ "version", no_argument, 0, 'v' },
				{ "user_timestamp", no_argument, 0, MAKE_WORD_FROM_CHAR('u', 't') },
				{ 0, 0, 0, 0 } };

		c = getopt_long_only(argc, argv, "hv", options, &optIdx);

		if (c == -1)
			break;

		switch (c)
		{
		case 1:
			if (inet_pton(AF_INET, optarg, outParams->ipAddr[ST_PPORT][ST_TX]) != 1)
			{
				printf("Error - exiting with code: 127\n");
				printf("\tCause: %s is not IP for pport_tx\n", optarg);
				exit(127);
			}
			memcpy((uint8_t *)&stParamVal.valueU32, outParams->ipAddr[ST_PPORT][ST_TX], 4);
			StSetParam(ST_DESTINATION_IP_TX, stParamVal);
			break;

		case 2:
			if (inet_pton(AF_INET, optarg, outParams->sipAddr[ST_PPORT]) != 1)
			{
				printf("Error - exiting with code: 127\n");
				printf("\tCause: %s is not IP\n", optarg);
				exit(127);
			}
			memcpy((uint8_t *)&stParamVal.valueU32, outParams->sipAddr[ST_PPORT], 4);
			StSetParam(ST_SOURCE_IP, stParamVal);
			break;

		case 3:
			outParams->pTx = 1;
			stParamVal.valueU64 = 1;
			StSetParam(ST_TX_FROM_P, stParamVal);
			break;

		case 4:
			outParams->pRx = 1;
			stParamVal.valueU64 = 1;
			StSetParam(ST_RX_FROM_P, stParamVal);
			break;
		case 5:
			outParams->rTx = 1;
			stParamVal.valueU64 = 1;
			StSetParam(ST_TX_FROM_R, stParamVal);
			break;
		case 6:
			outParams->rRx = 1;
			stParamVal.valueU64 = 1;
			StSetParam(ST_RX_FROM_R, stParamVal);
			break;

		case 7:
			if (inet_pton(AF_INET, optarg, outParams->ipAddr[ST_RPORT][ST_TX]) != 1)
			{
				printf("Error - exiting with code: 127\n");
				printf("\tCause: %s is not IP for rport_tx\n", optarg);
				exit(127);
			}
			memcpy((uint8_t *)&stParamVal.valueU32, outParams->ipAddr[ST_RPORT][ST_TX], 4);
			StSetParam(ST_RDESTINATION_IP_TX, stParamVal);
			break;

		case 8:
			if (inet_pton(AF_INET, optarg, outParams->sipAddr[ST_RPORT]) != 1)
			{
				printf("Error - exiting with code: 127\n");
				printf("\tCause: %s is not IP\n", optarg);
				exit(127);;
			}
			memcpy((uint8_t *)&stParamVal.valueU32, outParams->sipAddr[ST_RPORT], 4);
			StSetParam(ST_RSOURCE_IP, stParamVal);
			break;

		case 'c':
			outParams->appSCoreId = atoi(optarg);
			break;

		case MAKE_WORD_FROM_CHAR('c', 'l'):
			strLengh = strlen(optarg) + 2;
			exitStr = calloc(strLengh, sizeof(char));
			token = strtok(optarg, ",");
			if(exitStr == NULL)
			{
					printf("ERR: Invalid lib_score, not enough memory \n");
					exit(127);
			}
			while(token != NULL)
			{
				lib_core = strtol(token, &ptr, 10);
				if(*ptr != '\0' || lib_core >= UINT8_MAX || lib_core <= 0)
				{
					printf("ERR: Invalid lib_cid, only UIN8_T intigers are allowed\n");
					exit(127);
				}

				strncat(exitStr, token, sizeof(char) * 3);
				strcat(exitStr, ",");

				token = strtok(NULL, ",");
			}

			if(exitStr)exitStr[strlen(exitStr) - 1] = '\0';
			stParamVal.strPtr = exitStr;
			StSetParam(ST_LIB_SCOREID, stParamVal);
			free(exitStr);
			break;

		case 9:
			outParams->showframeInTx = 1;
			break;

		case 10:
			if (inet_pton(AF_INET, optarg, outParams->ipAddr[ST_PPORT][ST_RX]) != 1)
			{
				printf("Error - exiting with code: 127\n");
				printf("\tCause: %s is not IP for pport_rx\n", optarg);
				exit(127);
			}
			memcpy((uint8_t *)&stParamVal.valueU32, outParams->ipAddr[ST_PPORT][ST_RX], 4);
			StSetParam(ST_DESTINATION_IP_RX, stParamVal);
			break;

		case 11:
			if (inet_pton(AF_INET, optarg, outParams->ipAddr[ST_RPORT][ST_RX]) != 1)
			{
				printf("Error - exiting with code: 127\n");
				printf("\tCause: %s is not IP for rport_rx\n", optarg);
				exit(127);
			}
			memcpy((uint8_t *)&stParamVal.valueU32, outParams->ipAddr[ST_RPORT][ST_RX], 4);
			StSetParam(ST_RDESTINATION_IP_RX, stParamVal);
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
		case MAKE_WORD_FROM_CHAR('s', '3'):
			outParams->sn30Count = atoi(optarg);
			stParamVal.valueU64 = outParams->sn30Count;
			StSetParam(ST_SN30_COUNT, stParamVal);
			break;

		case MAKE_WORD_FROM_CHAR('s', '4'):
			outParams->sn40Count = atoi(optarg);
			stParamVal.valueU64 = outParams->sn40Count;
			StSetParam(ST_SN40_COUNT, stParamVal);
			break;

		case MAKE_WORD_FROM_CHAR('p', 'i'):	 // ptp clock id
		{
			st_ptp_clock_id_t clockId;
			if (sscanf(optarg, "%2hhx%2hhx%2hhx.%2hhx%2hhx.%2hhx%2hhx%2hhx", clockId.id + 0,
					   clockId.id + 1, clockId.id + 2, clockId.id + 3, clockId.id + 4,
					   clockId.id + 5, clockId.id + 6, clockId.id + 7)
				== 8)
			{
				stParamVal.ptr = (void *)&clockId;
				StPtpSetParam(ST_PTP_CLOCK_ID, stParamVal);
				stParamVal.valueU32 = ST_PTP_SET_MASTER;
				StPtpSetParam(ST_PTP_CHOOSE_CLOCK_MODE, stParamVal);
			}
			break;
		}
		case MAKE_WORD_FROM_CHAR('p', 'm'):
		{
			char m = optarg[0];
			if (m != 'm' && m != 'u')
				break;
			stParamVal.valueU32 = m == 'm' ? ST_PTP_MULTICAST_ADDR : ST_PTP_UNICAST_ADDR;
			StPtpSetParam(ST_PTP_ADDR_MODE, stParamVal);
			break;
		}
		case MAKE_WORD_FROM_CHAR('p', 's'):
		{
			char s = optarg[0];
			if (s != 'o' && s != 't')
				break;
			stParamVal.valueU32 = s == 't' ? ST_PTP_TWO_STEP : ST_PTP_ONE_STEP;
			StPtpSetParam(ST_PTP_STEP_MODE, stParamVal);
			break;
		}

		case MAKE_WORD_FROM_CHAR('b', 'n'):
			stParamVal.valueU64 = atoi(optarg);
			StSetParam(ST_BULK_NUM, stParamVal);
			break;

		case MAKE_WORD_FROM_CHAR('e', 't'):
			stParamVal.valueU64 = atoi(optarg);
			StSetParam(ST_ENQUEU_THREADS, stParamVal);
			break;

		case 'o':
			outParams->pPortName = optarg;
			stParamVal.strPtr = outParams->pPortName;
			StSetParam(ST_P_PORT, stParamVal);
			break;

		case 'i':
			outParams->rPortName = optarg;
			stParamVal.strPtr = outParams->rPortName;
			StSetParam(ST_R_PORT, stParamVal);
			break;
		case 'f':
		{
			char interlaced;
			if (sscanf(optarg, "%1c%u%1c%u", &isIntel, &height, &interlaced, &outParams->rate) == 4)
			{
				switch (isIntel)
				{
				default:
					printf("Error - exiting with code: 127\n");
					printf("\tCause: Invalid prefix used, allowed: a, i\n");
					exit(127);
				case 'a':
				case 'i':;
				}
				switch (height)
				{
				default:
					printf("Error - exiting with code: 127\n");
					printf("\tCause: Invalid frame heigth used, allowed: 720, 1080, 2160\n");
					exit(127);
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
				switch (interlaced)
				{
				default:
					printf("Error - exiting with code: 127\n");
					printf("\tCause: Invalid interlaced used, allowed: i, p\n");
					exit(127);
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
					printf("Error - exiting with code: 127\n");
					printf("\tCause: Invalid rate, allowed: 25, 29,50, 59\n");
					exit(127);
				case 25:
				case 29:
				case 50:
				case 59:;
				}
			}
			else
			{
				printf("Error - exiting with code: 127\n");
				printf("\tCause: Invalid format, example: a1080p29\n");
				exit(127);
			}
			stParamVal.valueU64 = outParams->fmtIndex;
			StSetParam(ST_FMT_INDEX, stParamVal);
		}
		break;

		case 'a':
			stParamVal.valueU64 = 0;
			StSetParam(ST_AUDIOFMT_INDEX, stParamVal);

		case 'p':
			outParams->udpPort = atoi(optarg);
			break;

		case MAKE_WORD_FROM_CHAR('v', 'f'):
			outParams->videoFileName = optarg;
			outParams->bufFormat = ST21_BUF_FMT_YUV_422_10BIT_BE;
			break;

		case MAKE_WORD_FROM_CHAR('a', 'f'):
			outParams->audioFileName = optarg;
			break;

		case MAKE_WORD_FROM_CHAR('c', 'f'):
			outParams->anciliaryFileName = optarg;
			break;

		case MAKE_WORD_FROM_CHAR('a', 's'):
		{
			int userArg_audioFrameSize = atoi(optarg);
			if ((userArg_audioFrameSize > 0) && (userArg_audioFrameSize <= ST_MAX_AUDIO_PKT_SIZE))
			{
				stParamVal.valueU32 = outParams->audioFrameSize = (uint16_t)userArg_audioFrameSize;
				StSetParam(ST_AUDIO_FRAME_SIZE, stParamVal);
			}
		}
		break;

		case MAKE_WORD_FROM_CHAR('p', 'c'):
			stParamVal.strPtr = optarg;
			StSetParam(ST_PACING_TYPE, stParamVal);
			break;

		case MAKE_WORD_FROM_CHAR('t', 'h'):
			stParamVal.valueU64 = atol(optarg);
			StSetParam(ST_TSC_HZ, stParamVal);
			break;

		case MAKE_WORD_FROM_CHAR('u', 't'):
			stParamVal.valueBool = true;
			StSetParam(ST_USER_TMSTAMP, stParamVal);
			break;

		case MAKE_WORD_FROM_CHAR('r', 'B'):
			stParamVal.valueU64 = atol(optarg);
			StSetParam(ST_RL_BPS, stParamVal);
			break;

		case 'h':
			PrintHelp();
			exit(0);

		case 'v':
			PrintVersion();
			exit(0);

		case '?':
			break;
		case 0:
			printf("Error - exiting with code: 0\n");
			printf("\tCause: Invalid arguments provided!\n");
			exit(0);
		default:
			PrintHelp();
			exit(0);
		}
		nargs = optind;
	}

	int numPorts = outParams->rPortName ? 2 : 1;
	stParamVal.valueU32 = outParams->numPorts = numPorts;
	StSetParam(ST_NUM_PORT, stParamVal);

	/* Verify if args were consistent
	 */
	if (outParams->fmtIndex >= ST21_FMT_MAX)
	{
		PrintHelp();
		printf("Error - exiting with code: %d\n", ST_FMT_ERR_BAD_HEIGHT);
		printf("\tCause: Invalid Format ID used");
		exit(ST_FMT_ERR_BAD_HEIGHT);
	}
	printf("INFO USER1: Chosen FMT is %s%d%s%d\n", (isIntel == 'i') ? "intel " : "all ", height,
			(outParams->interlaced) ? "i" : "p", outParams->rate);

	if (nargs == argc)
	{
		printf("Error - exiting with code: %d\n", ST_GENERAL_ERR);
		printf("\tCause: Application exited because of wrong usage\n");
		exit(ST_GENERAL_ERR);
	}
	return nargs;
}

st_status_t
InitSt21Format(st_user_params_t userParams, st21_format_t **txFmtOut, st21_format_t **rxFmtOut,
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
	txFmt->frameTime = 1000000000.0 * txFmt->frmRateDen / txFmt->frmRateMul;
	rxFmt->frameTime = 1000000000.0 * rxFmt->frmRateDen / rxFmt->frmRateMul;
	*txFmtOut = txFmt;
	*rxFmtOut = rxFmt;

	return ST_OK;
}

st_status_t
InitSt30Format(st_user_params_t userParams, st30_format_t **txFmtOut, st30_format_t **rxFmtOut)
{
	if (userParams.sn30Count > 0)
	{
		/* 
		* Provide frame size based on user input
		*/
		*txFmtOut = &stereoPcm24bFmt;
		*rxFmtOut = &stereoPcm24bFmt;
	}
	return ST_OK;
}

st_status_t
InitSt40Format(st_user_params_t userParams, st40_format_t **txFmtOut, st40_format_t **rxFmtOut)
{
	if (userParams.sn40Count > 0)
	{
		*txFmtOut = &ancillaryDataFmt;
		*rxFmtOut = &ancillaryDataFmt;
	}
	return ST_OK;
}

char *
SelectFile(uint8_t bufFormat, char *userFileName)
{
	if (userFileName != NULL)
		return userFileName;
	else
	{
		switch (bufFormat)
		{
		case ST30_BUF_FMT_WAV:
			return ST_DEFAULT_AUDIO;
		case ST21_BUF_FMT_RGBA_8BIT:
			return ST_DEFAULT_VIDEO_RGBA;
		case ST21_BUF_FMT_YUV_422_10BIT_BE:
			return ST_DEFAULT_VIDEO_YUV;
		case ST40_BUF_FMT_CLOSED_CAPTIONS:
			return ST_DEFAULT_ANCILIARY;
		}
	}
	return NULL;
}

void
SetupAppFmt(st21_format_t *vfmt, st30_format_t *afmt, st40_format_t *ancfmt)
{
	int i;
	st_format_t *fmt;

	/* TODO
      * We assume all video format is the same now, and need to enhance it to allow different formats
      */
	for (i = 0; i < rxtx_main.st21_session_count; i++)
	{
		fmt = &rxtx_main.fmt_lists[i];
		fmt->mtype = ST_ESSENCE_VIDEO;
		fmt->v = *vfmt;
	}

	for (i = 0; i < rxtx_main.st30_session_count; i++)
	{
		fmt = &rxtx_main.fmt_lists[i + rxtx_main.st21_session_count];
		fmt->mtype = ST_ESSENCE_AUDIO;
		fmt->a = *afmt;
	}

	for (i = 0; i < rxtx_main.st40_session_count; i++)
	{
		fmt = &rxtx_main.fmt_lists[i + rxtx_main.st21_session_count + rxtx_main.st30_session_count];
		fmt->mtype = ST_ESSENCE_ANC;
		fmt->anc = *ancfmt;
	}

	return;
}

st_status_t
InitTransmitter(st_user_params_t userParams, st_device_t **txDevOut, st_device_t confTx)
{
	st_status_t stat = ST_OK;

	st_device_t *txDev = NULL;

	/// Create TX device
	stat = StCreateDevice(&confTx, userParams.pPortName, userParams.rPortName, &txDev);
	if (stat != ST_OK)
	{
		printf("ERR USER1: StCreateDevice TX FAILED. ErrNo: %d\n", stat);
		return stat;
	}
	printf("INFO USER1: Create TX device done\n");

	rxtx_main.st21_session_count = txDev->snCount;
	rxtx_main.st30_session_count = txDev->sn30Count;
	rxtx_main.st40_session_count = txDev->sn40Count;

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
	stat = StCreateDevice(&confRx, userParams.pPortName, userParams.rPortName, &rxDev);
	if (stat != ST_OK)
	{
		printf("ERR USER1: StCreateDevice RX FAILED. ErrNo: %d\n", stat);
		return stat;
	}
	printf("INFO USER1: Create RX device done\n");

	rxtx_main.st21_session_count = rxDev->snCount;
	rxtx_main.st30_session_count = rxDev->sn30Count;
	rxtx_main.st40_session_count = rxDev->sn40Count;

	/// Return handle to the created device
	*rxDevOut = rxDev;

	return stat;
}

st_status_t
StartTransmitter(st_user_params_t userParams, st_session_t **txSnOut, st_device_t *txDev, strtp_send_app_t **txAppOut)
{
	st_status_t stat = ST_OK;
	char *fileName = NULL;
	strtp_send_app_t *txApp[txDev->snCount + txDev->sn30Count + txDev->sn40Count];

	st_session_t *txSn[txDev->snCount + txDev->sn30Count + txDev->sn40Count];
	bool isSendView = DoesGuiExist() && (userParams.showframeInTx == 1);

	AppInitAffinity(userParams.appSCoreId);

	/// Loop for create sessions
	for (uint32_t i = 0;
		 (i < txDev->snCount + txDev->sn30Count + txDev->sn40Count) && (userParams.pTx == 1 || userParams.rTx == 1);
		 i++)
	{
		/// Input parameters used by \ref StCreateSession
		st_session_t txSnIn = { 0 };
		st_addr_t txAddr;
		txSn[i] = NULL;
		txApp[i] = NULL;
		txSnIn.nicPort[ST_PPORT] = txDev->port[ST_PPORT];
		txSnIn.nicPort[ST_RPORT] = txDev->port[ST_RPORT];
		txSnIn.caps = ST_SN_DUAL_PATH | ST_SN_UNICAST | ST_SN_CONNECTLESS;
		txSnIn.ssid = 0x123450 + i;
		uint8_t bufFmt = 0;
		if (i < txDev->snCount)
		{
			txSnIn.type = ST_ESSENCE_VIDEO;
			bufFmt = userParams.bufFormat;
			fileName = SelectFile(bufFmt, userParams.videoFileName);
		}
		else if (i >= txDev->snCount && i < (txDev->snCount + txDev->sn30Count))
		{
			txSnIn.type = ST_ESSENCE_AUDIO;
			bufFmt = ST30_BUF_FMT_WAV;
			fileName = SelectFile(bufFmt, userParams.audioFileName);
		}
		else if (i >= (txDev->snCount + txDev->sn30Count)
				 && i < (txDev->snCount + txDev->sn30Count + txDev->sn40Count))
		{
			txSnIn.type = ST_ESSENCE_ANC;
			bufFmt = ST40_BUF_FMT_CLOSED_CAPTIONS;
			fileName = SelectFile(bufFmt, userParams.anciliaryFileName);
		}

		if (fileName == NULL)
		{
			printf("ERR USER1: Input file not provided\n");
			return ST_GENERAL_ERR;
		}
		/// Create session with given parameters
		stat = StCreateSession(txDev, &txSnIn, &rxtx_main.fmt_lists[i], &txSn[i]);
		if (stat != ST_OK)
		{
			printf("ERR USER1: StCreateSession FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Input parameters used by \ref StBindIpAddr
		memset(&txAddr, 0, sizeof(txAddr));
		txAddr.src.addr4.sin_family = AF_INET;
		txAddr.src.addr4.sin_port = htons(userParams.udpPort + i);
		txAddr.dst.addr4.sin_port = htons(userParams.udpPort + i);
		for (int p = 0; p < userParams.numPorts; ++p)
		{
			st_param_val_t sipAddr;

			if (((userParams.pTx == 1) && (p == 0)) || ((userParams.rTx == 1) && (p == 1)))
			{
				StGetParam((0 == p) ? ST_SOURCE_IP : ST_RSOURCE_IP, &sipAddr);

				memcpy((uint8_t *)&txAddr.src.addr4.sin_addr.s_addr, &sipAddr.valueU32, 4);
				memcpy((uint8_t *)&txAddr.dst.addr4.sin_addr.s_addr, userParams.ipAddr[p][ST_TX], 4);

				/// Bind IP addresses with proper MAC and fill addresses in the flow table
				stat = StBindIpAddr(txSn[i], &txAddr, txDev->port[p]);
				if (stat != ST_OK)
				{
					printf("ERR USER1: StBindIpAddr FAILED. ErrNo: %d\n", stat);
					return stat;
				}
			}
		}

		/// Get content prepare send mechanism and \ref StRegisterProducer
		stat = SendAppCreateProducer(txSn[i], bufFmt, fileName, &txApp[i]);
		if (stat != ST_OK)
		{
			printf("ERR USER1: SendAppCreateProducer FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		txApp[i]->index = i;

		/// Create viewer to enable presenting transmitted content on the screen
		txApp[i]->videoStream = NULL;
		if ((txSn[i]->type == ST_ESSENCE_VIDEO) && isSendView)
		{
			char label[256];
			st_format_t vfmt;
			snprintf(label, sizeof(label), "SENDER: %d", userParams.udpPort + i);
			StGetFormat(txSn[i], &vfmt);
			st21_format_t *fmt = &vfmt.v;
			stat = AddStream(&txApp[i]->videoStream, label, userParams.bufFormat, fmt->width,
							 fmt->height);
			if (stat != ST_OK)
			{
				printf("ERR USER1: CreateView sender FAILED. ErrNo: %d\n", stat);
				return stat;
			}
		}

		/// Set transmitter ready for sending by call \ref StProducerStartFrame
		stat = SendAppStart(txSn[i], txApp[i]);
		if (stat != ST_OK)
		{
			printf("ERR USER1: SendAppStart FAILED. ErrNo: %d\n", stat);
			return stat;
		}

		/// Return handle to the created session
		txSnOut[i] = txSn[i];
		txApp[i] = txApp[i];
	}

	/// Run threads for generating frames and for sending them
	stat = StStartDevice(txDev);
	if (stat != ST_OK)
	{
		printf("ERR USER1: StStartDevice (TX) FAILED. ErrNo: %d\n", stat);
		return stat;
	}

	return stat;
}

st_status_t
StartReceiver(st_user_params_t userParams, st_session_t **rxSnOut, st_device_t *rxDev,
			  strtp_recv_app_t **rxAppOut)
{
	st_status_t stat = ST_OK;
	st_session_t *rxSn[rxDev->snCount + rxDev->sn30Count + rxDev->sn40Count];
	strtp_recv_app_t *rxApp[rxDev->snCount + rxDev->sn30Count + rxDev->sn40Count];
	st_format_t vfmt;

	vfmt.mtype = ST_ESSENCE_VIDEO;
	bool isRxView = DoesGuiExist();

	AppInitAffinity(userParams.appSCoreId);

	/// Loop for create sessions
	for (uint32_t i = 0;
		 (i < rxDev->snCount + rxDev->sn30Count + rxDev->sn40Count) && (userParams.pRx == 1 || userParams.rRx == 1);
		 i++)
	{
		/// Input parameters used by \ref StCreateSession
		st_session_t rxSnIn = { 0 };
		st_addr_t rxAddr;
		rxSn[i] = NULL;
		rxApp[i] = NULL;

		rxSnIn.nicPort[ST_PPORT] = rxDev->port[ST_PPORT];
		rxSnIn.nicPort[ST_RPORT] = rxDev->port[ST_RPORT];
		rxSnIn.caps = ST_SN_DUAL_PATH | ST_SN_UNICAST | ST_SN_CONNECTLESS;
		rxSnIn.ssid = 0x123450 + i;

		uint8_t bufFmt = 0;
		if (i < rxDev->snCount)
		{
			rxSnIn.type = ST_ESSENCE_VIDEO;
			bufFmt = userParams.bufFormat;
		}
		else if (i >= rxDev->snCount && i < (rxDev->snCount + rxDev->sn30Count))
		{
			rxSnIn.type = ST_ESSENCE_AUDIO;
			bufFmt = ST30_BUF_FMT_WAV;
		}
		else if (i >= (rxDev->snCount + rxDev->sn30Count)
				 && i < (rxDev->snCount + rxDev->sn30Count + rxDev->sn40Count))
		{
			rxSnIn.type = ST_ESSENCE_ANC;
			bufFmt = ST40_BUF_FMT_CLOSED_CAPTIONS;
		}

		rxSnIn.timeslot = i;

		/// Create session with given parameters
		stat = StCreateSession(rxDev, &rxSnIn, &rxtx_main.fmt_lists[i], &rxSn[i]);
		if (stat != ST_OK)
		{
			printf("ERR USER1: StCreateSession FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		rxSn[i]->timeslot = rxSnIn.timeslot;
		/// Input parameters used by \ref StBindIpAddr
		memset(&rxAddr, 0, sizeof(rxAddr));
		rxAddr.src.addr4.sin_family = AF_INET;
		rxAddr.src.addr4.sin_port = htons(userParams.udpPort + rxSn[i]->timeslot);
		rxAddr.dst.addr4.sin_port = htons(userParams.udpPort + rxSn[i]->timeslot);
		for (int p = 0; p < userParams.numPorts; ++p)
		{

			st_param_val_t sipAddr;
			if (((userParams.pRx == 1) && (p == 0)) || ((userParams.rRx == 1) && (p == 1)))
			{
				StGetParam((0 == p) ? ST_SOURCE_IP : ST_RSOURCE_IP, &sipAddr);

				memcpy((uint8_t *)&rxAddr.src.addr4.sin_addr.s_addr, &sipAddr, 4);
				memcpy((uint8_t *)&rxAddr.dst.addr4.sin_addr.s_addr, userParams.ipAddr[p][ST_RX], 4);

				/// Bind IP addresses with proper MAC and fill addresses in the flow table
				stat = StBindIpAddr(rxSn[i], &rxAddr, rxDev->port[p]);
				if (stat != ST_OK)
				{
					printf("ERR USER1: StBindIpAddr FAILED. ErrNo: %d\n", stat);
					return stat;
				}
			}
		}
		/// Prepare receive mechanism and \ref StRegisterConsumer
		stat = RecvAppCreateConsumer(rxSn[i], bufFmt, &rxApp[i]);
		if (stat != ST_OK)
		{
			printf("ERR USER1: RecvAppCreateConsumer FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		rxApp[i]->index = i;

		if ((rxSn[i]->type == ST_ESSENCE_VIDEO) && isRxView)
		{
			/// Create viewer to enable presenting transmitted content on the screen
			rxApp[i]->videoStream = NULL;
			char label[256];

			snprintf(label, sizeof(label), "RECEIVER: %d", ntohs(rxAddr.src.addr4.sin_port));
			StGetFormat(rxSn[i], &vfmt);
			st21_format_t *fmt = &vfmt.v;
			stat = AddStream(&rxApp[i]->videoStream, label, userParams.bufFormat, fmt->width,
							 fmt->height);
			if (stat != ST_OK)
			{
				printf("ERR USER1: AddStream receiver FAILED. ErrNo: %d\n", stat);
				return stat;
			}
		}
		/// Set receiver ready for receive by call \ref St21ConsumerStartFrame
		stat = RecvAppStart(rxSn[i], rxApp[i]);
		if (stat != ST_OK)
		{
			printf("ERR USER1: RecvAppStart FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		rxSnOut[i] = rxSn[i];
		rxAppOut[i] = rxApp[i];
	}

	/// Run threads for receiving frames
	stat = StStartDevice(rxDev);
	if (stat != ST_OK)
	{
		printf("ERR USER1: StStartDevice (RX) FAILED. ErrNo: %d\n", stat);
		return stat;
	}

	return stat;
}

st_status_t
FinishTransmitter(st_session_t **txSnOut, uint32_t snTxCount, strtp_send_app_t **app)
{
	st_status_t stat = ST_OK;
	// Destroy TX session
	for (uint32_t i = 0; i < snTxCount; i++)
	{
		/// Finish sending frames
		stat = StProducerStop(txSnOut[i]);
		if (stat != ST_OK)
		{
			printf("ERR USER1: St21ProducerStop FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		/// Destroy transmitter session
		stat = StDestroySession(txSnOut[i]);
		numa_free(app[i], sizeof(strtp_send_app_t));
		app[i] = NULL;
		if (stat != ST_OK)
		{
			printf("ERR USER1: StDestroySession FAILED. ErrNo: %d\n", stat);
			return stat;
		}
	}
	printf("INFO USER1: Producer STOPPED and destroyed\n");
	// Destroy TX session end

	return stat;
}

st_status_t
FinishReceiver(st_session_t **rxSnOut, uint32_t snRxCount, strtp_recv_app_t **app)
{
	st_status_t stat = ST_OK;
	// Destroy RX session
	for (uint32_t i = 0; i < snRxCount; i++)
	{
		/// Finish receiving frames
		stat = RecvAppStop(rxSnOut[i], app[i]);
		if (stat != ST_OK)
		{
			printf("ERR USER1: St21ConsumerStop FAILED. ErrNo: %d\n", stat);
			return stat;
		}
		/// Destroy receiver session
		stat = StDestroySession(rxSnOut[i]);
		numa_free(app[i], sizeof(strtp_recv_app_t));
		app[i] = NULL;
		if (stat != ST_OK)
		{
			printf("ERR USER1: StDestroySession FAILED. ErrNo: %d\n", stat);
			return stat;
		}
	}
	printf("INFO USER1: Consumer STOPPED and destroyed\n");
	// Destroy RX session end

	return stat;
}

// Function created because cleaning huge pages using rte_eal_cleanup() doesn't work properly.
st_status_t
clearHugePages()
{
	st_status_t status = ST_OK;
	char filepath[280];
	char fileName[35];
	DIR *d;
	struct dirent *dir;

	d = opendir("/dev/hugepages/");
	snprintf(fileName, 35, "%smap_", ST_PREFIX_APPNAME);

	if (d)
	{
		while ((dir = readdir(d)) != NULL)
		{
			snprintf(filepath, 280, "/dev/hugepages/%s", dir->d_name);
			char *ret = strstr(filepath, fileName);
			if (ret != NULL)
			{
				status = access(filepath, W_OK);
				if (status)
				{
					printf("ERR USER1: Access to the rtemap (%s) failed! %s\n", filepath,
							strerror(errno));
					return ST_GENERAL_ERR;
				}

				status = remove(filepath);
				if (status != ST_OK)
				{
					printf("ERR USER1: Attempting to free Hugepages failed. Err: %s\n",
							strerror(errno));
					return ST_GENERAL_ERR;
				}

				printf("WARNING USER1: remove old mmap file (%s)\n", filepath);
			}
		}
		closedir(d);
	}

	return status;
}

/** Main point of the entire solution
* This is place where program is started
*/
int
main(int argc, char *argv[])
{
	st_status_t status = ST_OK;

	/* Initialization of variables
     */
	st_user_params_t userParams = { 0 };
	uint32_t snTxCount = 0;
	uint32_t snRxCount = 0;
	st21_format_t *txFmt = NULL;
	st21_format_t *rxFmt = NULL;
	st30_format_t *txAfmt = NULL;
	st30_format_t *rxAfmt = NULL;
	st40_format_t *txAncFmt = NULL;
	st40_format_t *rxAncFmt = NULL;
	st_device_t *txDev = NULL;
	st_device_t *rxDev = NULL;
	bool direct_free = false;

	/*
	 * STEP 1 - Preparing configuration for device initialization
	 */
	st_device_t confRx = {
		.type = ST_DEV_TYPE_CONSUMER,
		.exactRate = ST_DEV_RATE_P_29_97,
	};
	st_device_t confTx = {
		.type = ST_DEV_TYPE_PRODUCER,
		.exactRate = ST_DEV_RATE_P_29_97,
	};

	memset(&rxtx_main, 0, sizeof(rxtxapp_main_t));

	st_param_val_t stParamVal;
	userParams.rate = 29;
	userParams.fmtIndex = 1;
	userParams.udpPort = 10000;
	/* Default configuration is single Video session, no audio, no ancillary */
	userParams.snCount = 1;
	userParams.sn30Count = 0;
	userParams.sn40Count = 0;
	userParams.bufFormat = ST21_BUF_FMT_YUV_422_10BIT_BE;
	stParamVal.valueU64 = userParams.fmtIndex;
	StSetParam(ST_FMT_INDEX, stParamVal);

	printf("INFO USER1: Application %s started, cleaning previously used hugepages if any!\n",
			ST_PREFIX_APPNAME);
	status = clearHugePages();
	if (ST_OK != status)
	{
		printf("ERR USER1: Failed to cleanup used Pages. ErrNo: %d\n", status);
		return status;
	}

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

	// the option p_tx or p_rx is required
	if (userParams.pTx == 0 && userParams.pRx == 0 && userParams.rTx == 0 && userParams.rRx == 0)
	{
		if (userParams.numPorts >= 1 && userParams.pPortName != NULL)
		{
			userParams.pTx = 1;
			userParams.pRx = 1;
			stParamVal.valueU64 = 1;
			StSetParam(ST_TX_FROM_P, stParamVal);
			StSetParam(ST_RX_FROM_P, stParamVal);
			if (userParams.numPorts == MAX_RXTX_PORTS && userParams.rPortName != NULL)
			{
				userParams.rTx = 1;
				userParams.rRx = 1;
				stParamVal.valueU64 = 1;
				StSetParam(ST_TX_FROM_R, stParamVal);
				StSetParam(ST_RX_FROM_R, stParamVal);
			}
		}
		else
		{
			printf("ERR, USER1, The option --p_port is required.\n");
			return ST_GENERAL_ERR;
		}
	}
	if ((userParams.pTx == 1) && (*((uint32_t *)userParams.ipAddr[ST_PPORT][ST_TX]) == 0))
	{
		printf("ERR, USER1, p port is used for tx, but ip is not set");
		return ST_GENERAL_ERR;
	}
	if ((userParams.pRx == 1) && (*((uint32_t *)userParams.ipAddr[ST_PPORT][ST_RX]) == 0))
	{
		printf("ERR, USER1, p port is used for rx, but ip is not set");
		return ST_GENERAL_ERR;
	}
	if ((userParams.rTx == 1) && (*((uint32_t *)userParams.ipAddr[ST_RPORT][ST_TX]) == 0))
	{
		printf("ERR, USER1, r port is used for tx, but ip is not set");
		return ST_GENERAL_ERR;
	}
	if ((userParams.rRx == 1) && (*((uint32_t *)userParams.ipAddr[ST_RPORT][ST_RX]) == 0))
	{
		printf("ERR, USER1, r port is used for rx, but ip is not set");
		return ST_GENERAL_ERR;
	}

	rxtx_main.st21_session_count = userParams.snCount;
	rxtx_main.st30_session_count = userParams.sn30Count;
	rxtx_main.st40_session_count = userParams.sn40Count;
	rxtx_main.fmt_count = rxtx_main.st30_session_count + rxtx_main.st21_session_count
						  + rxtx_main.st40_session_count;

	/** STEP 3 - Select proper format for transmit and receive.
	* Format is related with different essences, such as image parameters, audio parameters etc.
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
	status = InitSt21Format(userParams, &txFmt, &rxFmt, &confTx, &confRx);
	if (status != ST_OK)
	{
		printf("ERR USER1: FormatInit FAILED. ErrNo: %d\n", status);
		return status;
	}

	status = InitSt30Format(userParams, &txAfmt, &rxAfmt);
	if (status != ST_OK)
	{
		printf("ERR USER1: FormatInit FAILED. ErrNo: %d\n", status);
		return status;
	}

	status = InitSt40Format(userParams, &txAncFmt, &rxAncFmt);
	if (status != ST_OK)
	{
		printf("ERR USER1: FormatInit FAILED. ErrNo: %d\n", status);
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
		printf("ERR USER1: InitTransmitter FAILED. ErrNo: %d\n", status);
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
		printf("ERR USER1: InitReceiver FAILED. ErrNo: %d\n", status);
		return status;
	}

	SetupAppFmt(txFmt, txAfmt, txAncFmt);

	st_session_t *rxSn[rxDev->snCount + rxDev->sn30Count + rxDev->sn40Count];
	st_session_t *txSn[txDev->snCount + txDev->sn30Count + txDev->sn40Count];
	strtp_recv_app_t *rxApp[rxDev->snCount + rxDev->sn30Count + rxDev->sn40Count];
	strtp_send_app_t *txApp [txDev->snCount + txDev->sn30Count + txDev->sn40Count];;
	/** STEP 6 - Initialization of Silmple DirectMedia Library
	* Library used for presenting transmitted content on the screen
	*
	* @return \ref st_status_t
	*/
	status = CreateGuiWindow();
	if (status != ST_OK)
	{
		printf("ERR USER1: InitSDL FAILED. ErrNo: %d\n", status);
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
	status = StartTransmitter(userParams, txSn, txDev, txApp);
	if (status != ST_OK)
	{
		direct_free = true;
		printf("ERR USER1: StartTransmitter FAILED. ErrNo: %d\n", status);
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
	status = StartReceiver(userParams, rxSn, rxDev, rxApp);
	if (status != ST_OK)
	{
		direct_free = true;
		printf("ERR USER1: StartReceiver FAILED. ErrNo: %d\n", status);
	}

	/** STEP 9 - API function responsible for get actual number of created transmitter sessions
	* Params:
	*	@param txDev - transmitter device (returned number of sessions will be number for this device)
	*	@param snTxCount - container for returned number of created sessions
	*
	* @return \ref st_status_t
	*/
	status = StGetSessionCount(txDev, &snTxCount);
	if (status != ST_OK)
	{
		direct_free = true;
		printf("ERR USER1: StGetSessionCount FAILED. ErrNo: %d\n", status);
	}

	printf("INFO USER1: Create TX sessions done. Number of sessions: %u\n", snTxCount);

	/** STEP 10 - API function responsible for get actual number of created receiver sessions
	* Params:
	*	@param rxDev - receiver device (returned number of sessions will be number for this device)
	*	@param snRxCount - container for returned number of created sessions
	*
	* @return \ref st_status_t
	*/
	status = StGetSessionCount(rxDev, &snRxCount);
	if (status != ST_OK)
	{
		direct_free = true;
		printf("ERR USER1: StGetSessionCount FAILED. ErrNo: %d\n", status);
	}

	printf("INFO USER1: Create RX sessions done. Number of sessions: %u\n", snRxCount);

	/** STEP 11 - Wait until signal caught
	* "pause" prevent before immediate finish and close transmission
	*/
	if (!direct_free)
	{
		pause();
	}
	/*
	 * dispaly accumulated status at exit
	 */
	StDisplayExitStats();

	/** STEP 12 - Stop transmitting and destroy transmitter sessions
	* Params:
	*	@param txSn - table of created transmitter sessions which will be closed and destroyed
	*	@param snTxCount - number of transmitter sessions
	*
	* @return \ref st_status_t
	*/
	status = FinishTransmitter(txSn, snTxCount, txApp);
	if (status != ST_OK)
	{
		printf("ERR USER1: FinishTransmitter FAILED. ErrNo: %d\n", status);
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
		printf("ERR USER1: FinishReceiver FAILED. ErrNo: %d\n", status);
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
		printf("ERR USER1: StDestroyDevice FAILED. ErrNo: %d\n", status);
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
		printf("ERR USER1: StDestroyDevice FAILED. ErrNo: %d\n", status);
	}

	/**
	 * STEP 16 -Destroy GUI
	 */
	DestroyGui();

	return status;
}
