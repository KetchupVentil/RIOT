/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file
 * @brief       
 * @author      Evgeniy Ponomarev
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "shell.h"
#include "shell_commands.h"
#include "thread.h"
#include "xtimer.h"
#include "lpm.h"
#include "periph/rtc.h"
#include "periph/gpio.h"
#include "random.h"

#include "sx1276.h"
#include "board.h"

#include "ls-end-device.h"
#include "unwds-common.h"
#include "unwds-gpio.h"

#include "main.h"
#include "config.h"
#include "utils.h"

typedef struct {
	bool is_valid;

	ls_channel_t channel;
	ls_datarate_t dr;

	uint8_t lnkchk_period;
	uint8_t max_retr;

	ls_node_class_t class;

	uint64_t ability;	/**< Defines ability mask - list of enabled UNWDS modules */
} node_role_settings_t;

static node_role_settings_t node_settings;

static sx1276_t sx1276;
static ls_ed_t ls;

static unsigned int join_retr_count;

void radio_init(void)
{
    sx1276.nss_pin = SX1276_SPI_NSS;
    sx1276.spi = SX1276_SPI;

    sx1276.dio0_pin = SX1276_DIO0;
    sx1276.dio1_pin = SX1276_DIO1;
    sx1276.dio2_pin = SX1276_DIO2;
    sx1276.dio3_pin = SX1276_DIO3;

    sx1276.dio4_pin = (gpio_t) NULL;
    sx1276.dio5_pin = (gpio_t) NULL;
    sx1276.reset_pin = (gpio_t) SX1276_RESET;

    sx1276_settings_t settings;
    settings.channel = RF_FREQUENCY;
    settings.modem = MODEM_LORA;
    settings.state = RF_IDLE;

    sx1276.settings = settings;

    puts("init_radio: sx1276 initialization done");
}

void lnkchk_timeout_cb(void) {
	puts("lnkchk: check failed, link is unavailable");

	ls_ed_join(&ls);
}

void link_good_cb(void) {
	puts("lnkchk: link good");

	blink_led();
}

void joined_timeout_cb(void) {
	puts("ls: join request timed out, resending");

    xtimer_usleep(1e6 * 2 * ++join_retr_count);
	ls_ed_join(&ls);
}


void joined_cb(void) {
	puts("ls-ed: successfully joined to the network");

	ls.status.batt_level = 0xAA;
	ls_ed_lnkchk(&ls);

	blink_led();
}

void appdata_send_failed_cb(void) {
	puts("ls-ed: application data confirmation timeout. Checking link...");

	ls_ed_lnkchk(&ls);
}

void appdata_received_cb(uint8_t *buf, size_t buflen) {
	char hex[100] = {};
	bytes_to_hex(buf, buflen, hex, false);

	printf("ls-ed: received data: \"%s\"\n", hex);
	blink_led();

	if (buflen < 2)
		return;

	unwds_module_id_t modid = buf[0];

	module_data_t cmd;
	memcpy(cmd.data, buf + 1, buflen - 1);
	cmd.length = buflen - 1;

	module_data_t reply = {};
	if (!unwds_send_to_module(modid, &cmd, &reply))
		return;

    int res = ls_ed_send_app_data(&ls, reply.data, reply.length, true);
    if (res < 0)
    	printf("send: error #%d\n", res);
}

static void standby_mode_cb(void) {
	puts("Peripherals disabled");
	xtimer_usleep(1000);

	gpio_clear(LED0_PIN);
	/* Disable Console UART */
	UART_0_CLKDIS();							/* Disable console UART clocking */
	SPI_1_CLKDIS();								/* Disable SPI clocking */
}

static void wakeup_cb(void) {
	UART_0_CLKEN();								/* Enable console UART clocking */
	SPI_1_CLKEN();								/* Disable SPI clocking */

	gpio_set(LED0_PIN);

	xtimer_usleep(1000);

	puts("Peripherals enabled");
}

static void ls_setup(ls_ed_t *ls)
{
	ls->settings.class = node_settings.class;

	ls->settings.dr = node_settings.dr;
	ls->settings.channel = node_settings.channel;

    ls->settings.app_id = config_get_appid();
    ls->settings.node_id = config_get_nodeid();

    memcpy(ls->settings.crypto.join_key, config_get_joinkey(), LS_MIC_KEY_LEN);

    ls->join_timeout_cb = joined_timeout_cb;
    ls->joined_cb = joined_cb;

    ls->link_good_cb = link_good_cb;
    ls->lnkchk_timeout_cb = lnkchk_timeout_cb;

    ls->appdata_send_failed_cb = appdata_send_failed_cb;
    ls->settings.max_retr = node_settings.max_retr;		/* Maximum number of confirmed data retransmissions */

    ls->appdata_received_cb = appdata_received_cb;

    ls->standby_mode_cb = standby_mode_cb;
    ls->wakeup_cb = wakeup_cb;

    ls->settings.lnkchk_failed_action = LS_ED_REJOIN;
    ls->settings.lnkchk_period_s = node_settings.lnkchk_period;

    ls->_internal.sx1276 = &sx1276;
}

int ls_set_cmd(int argc, char **argv) {
	if (argc != 3) {
		puts("usage: get <key> <value>");
		puts("keys:");
		puts("\tdr <0-7> -- sets device data rate [0 - slowest, 3 - average, 7 - fastest]");
		puts("\tch <0-2> -- sets device channel");
		puts("\tmaxretr <0-255> -- sets maximum number of retransmissions of confirmed app. data [5 is recommended]");
		puts("\tlnkchkperiod <1-255> -- sets link check period in seconds [120 is recommended]");

		//puts("\tclass <A/B> -- sets device class");
	}

	char* key = argv[1];
	char* value = argv[2];

	if (strcmp(key, "dr") == 0) {
		uint8_t v = strtol(value, NULL, 10);

		if (v > 7) {
			puts("set dr: datarate value must be from 0 to 7");
			return 1;
		}

		ls.settings.dr = (ls_datarate_t) v;
	} else if (strcmp(key, "ch") == 0) {
		uint8_t v = strtol(value, NULL, 10);

		if (v > 2) {
			puts("set ch: channel value must be from 0 to 2");
			return 1;
		}

		ls.settings.channel = (ls_channel_t) v;
	} else if (strcmp(key, "maxretr") == 0) {
		uint8_t v = strtol(value, NULL, 10);
		ls.settings.max_retr = v;
	} else if (strcmp(key, "lnkchkperiod") == 0) {
			uint8_t v = strtol(value, NULL, 10);
			if (v < 1) {
				puts("[error] Too small link check interval");
				return 1;
			}

			ls.settings.lnkchk_period_s = v;
	} else if (strcmp(key, "Class") == 0) {
		char v = value[0];

		if (v != 'A' && v != 'B') {
			puts("set Class: either A or B");
			return 1;
		}

		ls.settings.class = (v == 'A') ? LS_ED_CLASS_A : (v == 'B') ? LS_ED_CLASS_B : LS_ED_CLASS_B;
	}

	node_settings.channel = ls.settings.channel;
	node_settings.dr = ls.settings.dr;
	node_settings.lnkchk_period = ls.settings.lnkchk_period_s;
	node_settings.max_retr = ls.settings.max_retr;
	node_settings.class = ls.settings.class;

    return 0;
}

static void print_config(void) {
	puts("[ node configuration ]");

	uint64_t eui64 = config_get_nodeid();
	uint64_t appid = config_get_appid();

	printf("EUI64 = 0x%08x%08x\n", (unsigned int) (eui64 >> 32), (unsigned int) (eui64 & 0xFFFFFFFF));
	printf("APPID64 = 0x%08x%08x\n", (unsigned int) (appid >> 32), (unsigned int) (appid & 0xFFFFFFFF));

	printf("CHANNEL = %d\n", node_settings.channel);
	printf("DATARATE = %d\n", node_settings.dr);
	//printf("CLASS = %c\n", (node_settings.class == LS_ED_CLASS_A) ? 'A' : (node_settings.class == LS_ED_CLASS_B) ? 'B' : '?');
	printf("LNKCHKPERIOD (s) = %d\n", node_settings.lnkchk_period);
	printf("MAXRETR = %d\n", node_settings.max_retr);

	puts("[ enabled modules ]");
	unwds_list_modules(node_settings.ability, true);
}

static int ls_printc_cmd(int argc, char **argv) {
	print_config();

    return 0;
}

static int ls_save_cmd(int argc, char **argv) {

	puts("[*] Saving configuration...");
	node_settings.is_valid = true;
	if (!config_write_role_block((uint8_t *) &node_settings, sizeof(node_role_settings_t))) {
		puts("[error] Unable to save configuration");
	}

	puts("[done] Configuration saved. Type \"reboot\" to apply changes.");

	return 0;
}

int ls_cmd_cmd(int argc, char **argv) {
	if (argc < 2) {
		puts("Usage: cmd <modid> <cmdhex>");
		return 1;
	}

	uint8_t modid = atoi(argv[1]);
	if (!unwds_is_module_exists(modid)) {
		puts("cmd: module with specified id is not exists");
		return 1;
	}

	module_data_t cmd = {};

	int len = strlen(argv[2]);
	if (len % 2 != 0) {
		puts("cmd: command in hex must have an even length. Example: a0b100cc (4 bytes)");
		return 1;
	}

	if (len / 2 > UNWDS_MAX_DATA_LEN) {
		printf("cmd: command in hex is too long. Maximum is %d bytes long\n", UNWDS_MAX_DATA_LEN);
		return 1;
	}

	hex_to_bytes(argv[2], cmd.data, false);
	cmd.length = len / 2;

	module_data_t reply = {};
	bool res = unwds_send_to_module(modid, &cmd, &reply);
	char replystr[2 * UNWDS_MAX_DATA_LEN] = {};
	bytes_to_hex(reply.data, reply.length, replystr, true); // 007465737420737472696e670a

	if (res) {
		printf("[ok] Reply: %s\n", replystr);
	} else {
		printf("[fail] Reply: %s\n", replystr);
	}

	return 0;
}

static int ls_listmodules_cmd(int argc, char **argv) {
	puts("[ available modules ]");
	unwds_list_modules(node_settings.ability, false);

	return 0;
}

static int ls_modenable_cmd(int argc, char **argv) {
	if (argc < 1) {
		puts("Usage: modenable <modid>. Example: modenable 1");
		return 1;
	}

	uint8_t modid = atoi(argv[1]);
	if (!unwds_is_module_exists(modid)) {
		puts("modenable: module with specified id is not exists");
		return 1;
	}

	node_settings.ability |= unwds_get_ability_mask(modid);

	return 0;
}

static int ls_moddisable_cmd(int argc, char **argv) {
	if (argc < 1) {
		puts("Usage: modenable <modid>. Example: modenable 1");
		return 1;
	}

	uint8_t modid = atoi(argv[1]);
	if (!unwds_is_module_exists(modid)) {
		puts("moddisable: module with specified id is not exists");
		return 1;
	}

	/* Remove module from node's ability */
	node_settings.ability &= ~unwds_get_ability_mask(modid);

	return 0;
}

static const shell_command_t shell_commands[] = {
	{ "set", "<config> <value> -- sets up value for the config entry", ls_set_cmd },
	{ "listconfig", "-- prints out current configuration", ls_printc_cmd },

	{ "listmods", "-- list available modules", ls_listmodules_cmd },
	{ "modenable", "<modid> -- enables selected module", ls_modenable_cmd },
	{ "moddisable", "<modid> -- disables selected module", ls_moddisable_cmd },

	{ "save", "-- saves current configuration", ls_save_cmd },

	{ "cmd", "<modid> <cmdhex> -- executes command to against selected UNWDS module", ls_cmd_cmd },

    { NULL, NULL, NULL }
};

static void unwds_callback(module_data_t *buf) {
    int res = ls_ed_send_app_data(&ls, buf->data, buf->length, true);
    if (res < 0)
    {
    	if (res == -LS_SEND_E_FQ_OVERFLOW) {
    		puts("[error] Uplink queue overflowed!");
    	} if (res == -LS_SEND_E_NOT_JOINED) {
    		puts("[error] Cannot send app. data: not joined to the network");
    	} else
    		printf("send: error #%d\n", res);
    }

    blink_led();
}

static bool load_config(void) {
	if (!config_read_role_block((uint8_t *) &node_settings, sizeof(node_role_settings_t))) {
		puts("[node] Unable to load role specific configuration");

		return false;
	}

	return node_settings.is_valid;
}

void init_node(shell_command_t **commands)
{
	puts("[node] Initializing...");

    /* Set our commands for shell */
    memcpy(commands, shell_commands, sizeof(shell_commands));

    rtc_init();

    if (!load_config()) {
    	puts("[!] This device is not configured yet. Type \"help\" to see list of possible configuration commands.");
    	puts("[!] Configure this node and type \"reboot\" to reboot and apply settings.");

    	print_config();
    } else {
    	print_config();

        radio_init();
		ls_setup(&ls);
		ls_ed_init(&ls);

		wakeup_cb();

		unwds_set_ability(node_settings.ability);
		ls.settings.ability = node_settings.ability;
		ls.settings.class = LS_ED_CLASS_A;

		unwds_init_modules(unwds_callback);

		xtimer_usleep(1e6 * 1);
		ls_ed_join(&ls);

		blink_led();
    }
}

#ifdef __cplusplus
}
#endif