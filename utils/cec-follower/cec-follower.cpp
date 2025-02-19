// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
/*
 * Copyright 2016 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 */

#include <cstring>
#include <ctime>
#include <sstream>

#include <fcntl.h>
#include <getopt.h>
#include <sys/ioctl.h>

#include "cec-follower.h"
#include "compiler.h"

/* Short option list

   Please keep in alphabetical order.
   That makes it easier to see which short options are still free.

   In general the lower case is used to set something and the upper
   case is used to retrieve a setting. */
enum Option {
	OptSetAdapter = 'a',
	OptSetDevice = 'd',
	OptSetDriver = 'D',
	OptHelp = 'h',
	OptIgnore = 'i',
	OptNoWarnings = 'n',
	OptTrace = 'T',
	OptVerbose = 'v',
	OptShowMsgs = 'm',
	OptShowState = 's',
	OptWallClock = 'w',
	OptServiceByDigID = 128,
	OptStandby,
	OptTogglePowerStatus,
	OptVersion,
	OptLast = 256
};

static char options[OptLast];

bool show_info;
bool show_msgs;
bool show_state;
bool show_warnings = true;
unsigned warnings;

static struct option long_options[] = {
	{ "device", required_argument, 0, OptSetDevice },
	{ "adapter", required_argument, 0, OptSetAdapter },
	{ "driver", required_argument, 0, OptSetDriver },
	{ "help", no_argument, 0, OptHelp },
	{ "no-warnings", no_argument, 0, OptNoWarnings },
	{ "trace", no_argument, 0, OptTrace },
	{ "verbose", no_argument, 0, OptVerbose },
	{ "show-msgs", no_argument, 0, OptShowMsgs },
	{ "show-state", no_argument, 0, OptShowState },
	{ "wall-clock", no_argument, 0, OptWallClock },
	{ "service-by-dig-id", no_argument, 0, OptServiceByDigID },
	{ "standby", no_argument, 0, OptStandby },
	{ "toggle-power-status", required_argument, 0, OptTogglePowerStatus },
	{ "ignore", required_argument, 0, OptIgnore },
	{ "version", no_argument, 0, OptVersion },

	{ 0, 0, 0, 0 }
};

#define STR(x) #x
#define STRING(x) STR(x)

static void usage()
{
	printf("Usage:\n"
	       "  -d, --device <dev>  Use device <dev> instead of /dev/cec0\n"
	       "                      If <dev> starts with a digit, then /dev/cec<dev> is used.\n"
	       "  -D, --driver <driver>    Use a cec device with this driver name\n"
	       "  -a, --adapter <adapter>  Use a cec device with this adapter name\n"
	       "  -h, --help          Display this help message\n"
	       "  -n, --no-warnings   Turn off warning messages\n"
	       "  -T, --trace         Trace all called ioctls\n"
	       "  -v, --verbose       Turn on verbose reporting\n"
	       "  -w, --wall-clock    Show timestamps as wall-clock time (implies -v)\n"
	       "  -m, --show-msgs     Show received messages\n"
	       "  -s, --show-state    Show state changes from the emulated device\n"
	       "  --service-by-dig-id Report digital services by digital ID instead of by channel\n"
	       "  --standby           Start in Standby state\n"
	       "  --toggle-power-status <secs>\n"
	       "                      Toggle the power status every <secs> seconds\n"
	       "  -i, --ignore <la>,<opcode>\n"
	       "                      Ignore messages from logical address <la> and opcode\n"
	       "                      <opcode>. 'all' can be used for <la> or <opcode> to match\n"
	       "                      all logical addresses or opcodes.\n"
	       "  --version           Show version information\n"
	       );
}

void sad_encode(const struct short_audio_desc *sad, __u32 *descriptor)
{
	__u8 b1, b2, b3 = 0;

	b1 = (sad->num_channels - 1) & 0x07;
	b1 |= (sad->format_code & 0x0f) << 3;

	b2 = sad->sample_freq_mask;

	switch (sad->format_code) {
	case SAD_FMT_CODE_LPCM:
		b3 = sad->bit_depth_mask & 0x07;
		break;
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
		b3 = sad->max_bitrate;
		break;
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
		b3 = sad->format_dependent;
		break;
	case SAD_FMT_CODE_WMA_PRO:
		b3 = sad->wma_profile & 0x03;
		break;
	case SAD_FMT_CODE_EXTENDED:
		b3 = (sad->extension_type_code & 0x1f) << 3;

		switch (sad->extension_type_code) {
		case 4:
		case 5:
		case 6:
			b3 |= (sad->frame_length_mask & 0x03) << 1;
			break;
		case 8:
		case 10:
			b3 |= (sad->frame_length_mask & 0x03) << 1;
			b3 |= sad->mps & 1;
			break;
		case SAD_EXT_TYPE_MPEG_H_3D_AUDIO:
		case SAD_EXT_TYPE_AC_4:
			b3 |= sad->format_dependent & 0x07;
			fallthrough;
		case SAD_EXT_TYPE_LPCM_3D_AUDIO:
			b3 |= sad->bit_depth_mask & 0x07;
			break;
		}
		break;
	}

	*descriptor = (b1 << 16) | (b2 << 8) | b3;
}

static std::string audio_format_code2s(__u8 format_code)
{
	switch (format_code) {
	case 0:
		return "Reserved";
	case SAD_FMT_CODE_LPCM:
		return "L-PCM";
	case SAD_FMT_CODE_AC3:
		return "AC-3";
	case SAD_FMT_CODE_MPEG1:
		return "MPEG-1";
	case SAD_FMT_CODE_MP3:
		return "MP3";
	case SAD_FMT_CODE_MPEG2:
		return "MPEG2";
	case SAD_FMT_CODE_AAC_LC:
		return "AAC LC";
	case SAD_FMT_CODE_DTS:
		return "DTS";
	case SAD_FMT_CODE_ATRAC:
		return "ATRAC";
	case SAD_FMT_CODE_ONE_BIT_AUDIO:
		return "One Bit Audio";
	case SAD_FMT_CODE_ENHANCED_AC3:
		return "Enhanced AC-3";
	case SAD_FMT_CODE_DTS_HD:
		return "DTS-HD";
	case SAD_FMT_CODE_MAT:
		return "MAT";
	case SAD_FMT_CODE_DST:
		return "DST";
	case SAD_FMT_CODE_WMA_PRO:
		return "WMA Pro";
	case SAD_FMT_CODE_EXTENDED:
		return "Extended";
	default:
		return "Illegal";
	}
}

std::string extension_type_code2s(__u8 type_code)
{
	switch (type_code) {
	case 0:
	case 1:
	case 2:
	case 3:
		return "Not in use";
	case SAD_EXT_TYPE_MPEG4_HE_AAC:
		return "MPEG-4 HE AAC";
	case SAD_EXT_TYPE_MPEG4_HE_AACv2:
		return "MPEG-4 HE AAC v2";
	case SAD_EXT_TYPE_MPEG4_AAC_LC:
		return "MPEG-4 AAC LC";
	case SAD_EXT_TYPE_DRA:
		return "DRA";
	case SAD_EXT_TYPE_MPEG4_HE_AAC_SURROUND:
		return "MPEG-4 HE AAC + MPEG Surround";
	case SAD_EXT_TYPE_MPEG4_AAC_LC_SURROUND:
		return "MPEG-4 AAC LC + MPEG Surround";
	case SAD_EXT_TYPE_MPEG_H_3D_AUDIO:
		return "MPEG-H 3D Audio";
	case SAD_EXT_TYPE_AC_4:
		return "AC-4";
	case SAD_EXT_TYPE_LPCM_3D_AUDIO:
		return "L-PCM 3D Audio";
	default:
		return "Reserved";
	}
}

std::string audio_format_id_code2s(__u8 audio_format_id, __u8 audio_format_code)
{
	if (audio_format_id == 0)
		return audio_format_code2s(audio_format_code);
	if (audio_format_id == 1)
		return extension_type_code2s(audio_format_code);
	return "Invalid";
}

std::string opcode2s(const struct cec_msg *msg)
{
	std::stringstream oss;
	__u8 opcode = msg->msg[1];
	const char *name;

	if (opcode == CEC_MSG_CDC_MESSAGE) {
		__u8 cdc_opcode = msg->msg[4];
		name = cec_cdc_opcode2s(cdc_opcode);

		if (name)
			return name;
		oss << "CDC: 0x" << std::hex << static_cast<unsigned>(cdc_opcode);
		return oss.str();
	}

	name = cec_opcode2s(opcode);

	if (name)
		return name;
	oss << "0x" << std::hex << static_cast<unsigned>(opcode);
	return oss.str();
}

int cec_named_ioctl(int fd, const char *name,
		    unsigned long int request, void *parm)
{
	int retval;
	int e;

	retval = ioctl(fd, request, parm);

	e = retval == 0 ? 0 : errno;
	if (options[OptTrace])
		printf("\t\t%s returned %d (%s)\n",
			name, retval, strerror(e));

	if (!retval) {
		const auto msg = static_cast<const struct cec_msg *>(parm);

		/* Update the timestamp whenever we successfully transmit to an LA,
		   or whenever we receive something from the LA */
		if (request == CEC_TRANSMIT && (msg->tx_status & CEC_TX_STATUS_OK) &&
		    !cec_msg_is_broadcast(msg)) {
			if (msg->timeout) {
				if (msg->rx_status & (CEC_RX_STATUS_OK | CEC_RX_STATUS_FEATURE_ABORT))
					la_info[cec_msg_initiator(msg)].ts = msg->rx_ts;
			} else
				la_info[cec_msg_destination(msg)].ts = msg->tx_ts;
		}
		if (request == CEC_RECEIVE &&
		    cec_msg_initiator(msg) != CEC_LOG_ADDR_UNREGISTERED &&
		    (msg->rx_status & CEC_RX_STATUS_OK))
			la_info[cec_msg_initiator(msg)].ts = msg->rx_ts;
	}

	return retval == -1 ? e : (retval ? -1 : 0);
}

void state_init(struct node &node)
{
	if (options[OptStandby])
		node.state.power_status = CEC_OP_POWER_STATUS_STANDBY;
	else
		node.state.power_status = CEC_OP_POWER_STATUS_ON;
	node.state.old_power_status = CEC_OP_POWER_STATUS_ON;
	node.state.power_status_changed_time = 0;
	strcpy(node.state.menu_language, "eng");
	node.state.video_latency = 10;
	node.state.low_latency_mode = 1;
	node.state.audio_out_compensated = 3;
	node.state.audio_out_delay = 20;
	node.state.arc_active = false;
	node.state.sac_active = false;
	node.state.volume = 50;
	node.state.mute = false;
	tuner_dev_info_init(&node.state);
}

int main(int argc, char **argv)
{
	std::string device;
	struct node node = { };
	const char *driver = NULL;
	const char *adapter = NULL;
	unsigned toggle_power_status = 0;
	char short_options[26 * 2 * 2 + 1];
	int idx = 0;
	int fd = -1;
	int ch;
	int i;

	for (i = 0; long_options[i].name; i++) {
		if (!isalpha(long_options[i].val))
			continue;
		short_options[idx++] = long_options[i].val;
		if (long_options[i].has_arg == required_argument) {
			short_options[idx++] = ':';
		} else if (long_options[i].has_arg == optional_argument) {
			short_options[idx++] = ':';
			short_options[idx++] = ':';
		}
	}
	while (true) {
		int option_index = 0;

		short_options[idx] = 0;
		ch = getopt_long(argc, argv, short_options,
				 long_options, &option_index);
		if (ch == -1)
			break;

		options[ch] = 1;
		if (!option_index) {
			for (i = 0; long_options[i].val; i++) {
				if (long_options[i].val == ch) {
					option_index = i;
					break;
				}
			}
		}
		if (long_options[option_index].has_arg == optional_argument &&
		    !optarg && argv[optind] && argv[optind][0] != '-')
			optarg = argv[optind++];

		switch (ch) {
		case OptHelp:
			usage();
			return 0;
		case OptSetDevice:
			device = optarg;
			if (device[0] >= '0' && device[0] <= '9' && device.length() <= 3) {
				static char newdev[20];

				sprintf(newdev, "/dev/cec%s", optarg);
				device = newdev;
			}
			break;
		case OptSetDriver:
			driver = optarg;
			break;
		case OptSetAdapter:
			adapter = optarg;
			break;
		case OptNoWarnings:
			show_warnings = false;
			break;
		case OptShowMsgs:
			show_msgs = true;
			break;
		case OptShowState:
			show_state = true;
			break;
		case OptTogglePowerStatus:
			toggle_power_status = strtoul(optarg, NULL, 0);
			break;
		case OptIgnore: {
			bool all_la = !strncmp(optarg, "all", 3);
			bool all_opcodes = true;
			const char *sep = std::strchr(optarg, ',');
			unsigned la_mask = 0xffff, opcode, la = 0;

			if (sep)
				all_opcodes = !strncmp(sep + 1, "all", 3);
			if (!all_la) {
				la = strtoul(optarg, NULL, 0);

				if (la > 15) {
					fprintf(stderr, "invalid logical address (> 15)\n");
					usage();
					return 1;
				}
				la_mask = 1 << la;
			}
			if (!all_opcodes) {
				opcode = strtoul(sep + 1, NULL, 0);
				if (opcode > 255) {
					fprintf(stderr, "invalid opcode (> 255)\n");
					usage();
					return 1;
				}
				node.ignore_opcode[opcode] |= la_mask;
				break;
			}
			if (all_la && all_opcodes) {
				fprintf(stderr, "all,all is invalid\n");
				usage();
				return 1;
			}
			node.ignore_la[la] = true;
			break;
		}
		case OptWallClock:
		case OptVerbose:
			show_info = true;
			show_msgs = true;
			show_state = true;
			break;
		case OptVersion:
			printf("cec-follower %s%s\n", PACKAGE_VERSION, STRING(GIT_COMMIT_CNT));
			printf("cec-follower SHA: %s\n", STRING(GIT_SHA));
			std::exit(EXIT_SUCCESS);
		case ':':
			fprintf(stderr, "Option '%s' requires a value\n",
				argv[optind]);
			usage();
			return 1;
		case '?':
			if (argv[optind])
				fprintf(stderr, "Unknown argument '%s'\n", argv[optind]);
			usage();
			return 1;
		}
	}
	if (optind < argc) {
		printf("unknown arguments: ");
		while (optind < argc)
			printf("%s ", argv[optind++]);
		printf("\n");
		usage();
		return 1;
	}

	if (device.empty() && (driver || adapter)) {
		device = cec_device_find(driver, adapter);
		if (device.empty()) {
			fprintf(stderr,
				"Could not find a CEC device for the given driver/adapter combination\n");
			std::exit(EXIT_FAILURE);
		}
	}
	if (device.empty())
		device = "/dev/cec0";

	if ((fd = open(device.c_str(), O_RDWR)) < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", device.c_str(),
			strerror(errno));
		std::exit(EXIT_FAILURE);
	}

	struct cec_caps caps = { };

	node.fd = fd;
	node.device = device.c_str();
	doioctl(&node, CEC_ADAP_G_CAPS, &caps);
	node.caps = caps.capabilities;
	node.available_log_addrs = caps.available_log_addrs;
	node.state.service_by_dig_id = options[OptServiceByDigID];
	node.state.toggle_power_status = toggle_power_status;
	state_init(node);

	printf("cec-follower SHA                   : %s\n", STRING(GIT_SHA));

	doioctl(&node, CEC_ADAP_G_PHYS_ADDR, &node.phys_addr);

	struct cec_log_addrs laddrs = { };
	doioctl(&node, CEC_ADAP_G_LOG_ADDRS, &laddrs);
	node.adap_la_mask = laddrs.log_addr_mask;
	node.cec_version = laddrs.cec_version;

	struct cec_connector_info conn_info = {};

	doioctl(&node, CEC_ADAP_G_CONNECTOR_INFO, &conn_info);

	cec_driver_info(caps, laddrs, node.phys_addr, conn_info);

	if (laddrs.cec_version >= CEC_OP_CEC_VERSION_2_0) {
		bool is_dev_feat = false;

		for (__u8 byte : laddrs.features[0]) {
			if (is_dev_feat) {
				node.source_has_arc_rx = (byte & CEC_OP_FEAT_DEV_SOURCE_HAS_ARC_RX) != 0;
				node.sink_has_arc_tx = (byte & CEC_OP_FEAT_DEV_SINK_HAS_ARC_TX) != 0;
				node.has_aud_rate = (byte & CEC_OP_FEAT_DEV_HAS_SET_AUDIO_RATE) != 0;
				node.has_deck_ctl = (byte & CEC_OP_FEAT_DEV_HAS_DECK_CONTROL) != 0;
				node.has_rec_tv = (byte & CEC_OP_FEAT_DEV_HAS_RECORD_TV_SCREEN) != 0;
				node.has_osd_string = (byte & CEC_OP_FEAT_DEV_HAS_SET_OSD_STRING) != 0;
				break;
			}
			if (byte & CEC_OP_FEAT_EXT)
				continue;
			if (!is_dev_feat)
				is_dev_feat = true;
			else
				break;
		}
	}
	printf("\n");

	bool missing_pa = node.phys_addr == CEC_PHYS_ADDR_INVALID &&
		(node.caps & CEC_CAP_PHYS_ADDR) &&
		!(node.caps & CEC_CAP_CONNECTOR_INFO);
	bool missing_la = laddrs.num_log_addrs == 0 && (node.caps & CEC_CAP_LOG_ADDRS);

	if (missing_la || missing_pa)
		printf("\n");
	if (missing_pa)
		fprintf(stderr, "FAIL: missing physical address, use cec-ctl to configure this\n");
	if (missing_la)
		fprintf(stderr, "FAIL: missing logical address(es), use cec-ctl to configure this\n");
	if (missing_la || missing_pa)
		std::exit(EXIT_FAILURE);

	testProcessing(&node, options[OptWallClock]);
}
