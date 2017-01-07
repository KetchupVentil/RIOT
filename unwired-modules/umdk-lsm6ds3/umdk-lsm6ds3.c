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
 * @file		umdk-acc.c
 * @brief       umdk-acc module implementation
 * @author      Eugene Ponomarev
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "periph/gpio.h"

#include "board.h"

#include "unwds-common.h"
#include "include/umdk-lsm6ds3.h"
#include "include/lsm6ds3.h"

#include "thread.h"
#include "xtimer.h"

static uwnds_cb_t *callback;

static lsm6ds3_t lsm6ds3;

void umdk_lsm6ds3_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback)
{
    (void) non_gpio_pin_map;

    callback = event_callback;

    lsm6ds3_param_t lsm_params;
    lsm_params.i2c_addr = 0x6A;
    lsm_params.i2c = UMDK_LSM6DS3_I2C;

    if (lsm6ds3_init(&lsm6ds3, &lsm_params) < 0) {
        puts("[umdk-acc] Initialization of LSM6DS3 failed");
    }

    /* Configure the default settings */
    lsm_params.gyro_enabled = true;
    lsm_params.gyro_range = 2000;
    lsm_params.gyro_sample_rate = 416;
    lsm_params.gyro_bandwidth = 400;
    lsm_params.gyro_fifo_enabled = true;
    lsm_params.gyro_fifo_decimation = true;

    lsm_params.accel_enabled = true;
    lsm_params.accel_odr_off = true;
    lsm_params.accel_range = 8;
    lsm_params.accel_sample_rate = 416;
    lsm_params.accel_bandwidth = 400;
    lsm_params.accel_fifo_enabled = true;
    lsm_params.accel_fifo_decimation = true;

    lsm_params.temp_enabled = true;

    lsm_params.comm_mode = 1;

    lsm_params.fifo_threshold = 3000;
    lsm_params.fifo_sample_rate = 10;
    lsm_params.fifo_mode_word = 0;

    if (!lsm6ds3_configure(&lsm6ds3, &lsm_params)) {
        puts("[umdk-acc] Configuration of LSM6DS3 failed");
    }
}

bool umdk_lsm6ds3_cmd(module_data_t *cmd, module_data_t *reply)
{
	/* Check for empty command */
	if (cmd->length < 1)
		return false;

	umdk_lsm6ds3_cmd_t c = cmd->data[0];

	switch (c) {
	case UMDK_LSM6DS3_CMD_POLL:
	{
		lsm6ds3_data_t acc_data = {};
        lsm6ds3_get_raw(&lsm6ds3, &acc_data);
		uint16_t temp = lsm6ds3_read_temp_c(&lsm6ds3);
		
		reply->length = 1 + sizeof(lsm6ds3_data_t) + 2;
		reply->data[0] = UNWDS_LSM6DS3_MODULE_ID;
		
		memcpy(reply->data + 1, &acc_data, sizeof(lsm6ds3_data_t));
		memcpy(reply->data + 1 + sizeof(lsm6ds3_data_t), &temp, 2);
		
		break;
	}
	default:
		return false;
	}
	
    return true;
}

#ifdef __cplusplus
}
#endif