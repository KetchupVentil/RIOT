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
 * @file		umdk-4btn.c
 * @brief       umdk-4btn module implementation
 * @author      Eugene Ponomarev
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include "periph/gpio.h"

#include "board.h"

#include "unwds-common.h"
#include "umdk-4btn.h"

#include "thread.h"
#include "xtimer.h"

static kernel_pid_t handler_pid;

#define UMDK_4BTN_NUM_BUTTONS 4
static gpio_t buttons[UMDK_4BTN_NUM_BUTTONS] = {UMDK_4BTN_1, UMDK_4BTN_2, UMDK_4BTN_3, UMDK_4BTN_4};

static uwnds_cb_t *callback;

static void *handler(void *arg) {
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);

    while (1) {
        msg_receive(&msg);
        int btn = msg.type;

        module_data_t data;
        data.length = 3;
        data.data[0] = UNWDS_4BTN_MODULE_ID;
        data.data[1] = (btn & 0xFF) + 1;
        data.data[2] = (btn >> 16);

        callback(&data);
    }

	return NULL;
}

static void btn_pressed_int(void *arg) {
	int btn_num = (int) arg;
    
    gpio_irq_disable(buttons[btn_num]);
    
    uint8_t now_value = 0;
    uint8_t last_value = gpio_read(buttons[btn_num]);

    uint8_t value_counter = 0;
    uint8_t error_counter = 0;
    
    do {
        xtimer_spin(xtimer_ticks_from_usec(10*1e3));
        now_value = gpio_read(buttons[btn_num]);
        
        if (now_value == last_value) {
            value_counter++;
            last_value = now_value;
        } else {
            value_counter = 0;
            error_counter++;
        }
    } while ((value_counter < 5) && (error_counter < 100));
    
    if (error_counter == 100) {
        puts("[4btn] Press rejected");
        gpio_irq_enable(buttons[btn_num]);
        return;
    }
    
    printf("[4btn] Pressed: %d\n", btn_num + 1);

    msg_t msg;
    msg.type = btn_num;
    if (last_value) {
        /* button released */
        msg.type |= 1 << 16;
    }

	msg_send_int(&msg, handler_pid);
    
    gpio_irq_enable(buttons[btn_num]);
}

void umdk_4btn_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback) {
	(void) non_gpio_pin_map;

	callback = event_callback;

	/* Initialize interrupts */
    int i = 0;
    for (i = 0; i < UMDK_4BTN_NUM_BUTTONS; i++) {
        gpio_init_int(buttons[i], GPIO_IN_PU, GPIO_BOTH, btn_pressed_int, (void *) i);
    }

	/* Create handler thread */
	char *stack = (char *) allocate_stack();
	if (!stack) {
		puts("umdk-4btn: unable to allocate memory. Is too many modules enabled?");
		return;
	}

	handler_pid = thread_create(stack, UNWDS_STACK_SIZE_BYTES, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, handler, NULL, "4btn thread");
}

bool umdk_4btn_cmd(module_data_t *data, module_data_t *reply) {
	return false;
}

#ifdef __cplusplus
}
#endif
