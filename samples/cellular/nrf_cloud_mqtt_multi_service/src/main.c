/* Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/logging/log.h>
#include "application.h"
#include "connection.h"
#include "led_control.h"

LOG_MODULE_REGISTER(main, CONFIG_MQTT_MULTI_SERVICE_LOG_LEVEL);

/* Here, we start the various threads that our application will run in */

#ifndef CONFIG_LED_INDICATION_DISABLED
/* Define, and automatically start the LED animation thread. See led_control.c */
K_THREAD_DEFINE(led_thread, CONFIG_LED_THREAD_STACK_SIZE, led_animation_thread_fn,
		NULL, NULL, NULL, 0, 0, 0);
#endif

/* Define, and automatically start the main application thread. See application.c */
//K_THREAD_DEFINE(app_thread, CONFIG_APPLICATION_THREAD_STACK_SIZE, main_application_thread_fn,
//		NULL, NULL, NULL, 0, 0, 0);

/* Define, and automatically start the message queue thread. See connection.c */
//K_THREAD_DEFINE(msg_thread, CONFIG_MESSAGE_THREAD_STACK_SIZE, message_queue_thread_fn,
//		NULL, NULL, NULL, 0, 0, 0);

/* Define, and automatically start the connection management thread. See connection.c
 *
 * The connection thread is given higher priority (-1) so that it can preempt the other threads,
 * for instance in the event of a call to disconnect_cloud().
 *
 * Priority -1 is also a non-preeemptible priority level, so other threads, even of higher
 * priority, cannot interrupt the connection thread until it yields.
 */
//K_THREAD_DEFINE(con_thread, CONFIG_CONNECTION_THREAD_STACK_SIZE, connection_management_thread_fn,
//		NULL, NULL, NULL, -1, 0, 0);

struct demonstration {
	int oh_no;
};

void test_func(int somevalue)
{
	printk("Am I executed? (1) \n");

	printk("I was passed %d \n", somevalue);

	printk("Am I executed? (2) \n");
}

void demo_crash(void)
{
	k_sleep(K_SECONDS(5));

	printk("I am about to purposefully crash!\n");

	struct demonstration *demo = NULL;

	test_func(demo->oh_no);

	printk("Am I executed? (3) \n");
}

int main(void)
{
	demo_crash();
	return 0;
}
