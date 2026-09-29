/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "host_power_profile.h"

LOG_MODULE_REGISTER(host_power_profile, LOG_LEVEL_INF);

#if defined(CONFIG_HOST_POWER_PROFILE_REGISTERED) || defined(CONFIG_HOST_POWER_PROFILE_PSM) || \
	defined(CONFIG_HOST_POWER_PROFILE_XSLEEP)

#include <sm_at_client.h>

#define COMMAND_TIMEOUT_SECONDS 10
#define MODEM_BOOT_DELAY_SECONDS 2
#define RESPONSE_BUFFER_SIZE 192

static K_SEM_DEFINE(response_complete, 0, 1);
static char response[RESPONSE_BUFFER_SIZE];
static size_t response_length;
static bool capture_response;
static int profile_result = -EAGAIN;

static void response_handler(const uint8_t *data, size_t length)
{
	if (!capture_response) {
		return;
	}

	size_t available = sizeof(response) - 1 - response_length;
	size_t copied = MIN(length, available);

	memcpy(response + response_length, data, copied);
	response_length += copied;
	response[response_length] = '\0';

	if (strstr(response, "\r\nOK\r\n") != NULL ||
	    strstr(response, "\r\nERROR\r\n") != NULL ||
	    strstr(response, "\r\n+CME ERROR:") != NULL ||
	    strstr(response, "\r\n+CMS ERROR:") != NULL) {
		k_sem_give(&response_complete);
	}
}

static int command_send(const char *command)
{
	int ret;

	capture_response = false;
	response_length = 0;
	response[0] = '\0';
	k_sem_reset(&response_complete);
	capture_response = true;

	ret = sm_at_client_send_cmd(command, COMMAND_TIMEOUT_SECONDS);
	if (ret != AT_CMD_OK) {
		capture_response = false;
		LOG_ERR("%s failed: %d", command, ret);
		return ret < 0 ? ret : -EIO;
	}

	ret = k_sem_take(&response_complete, K_SECONDS(1));
	capture_response = false;
	if (ret < 0) {
		LOG_ERR("Did not capture complete response to %s", command);
		return ret;
	}

	return 0;
}

static int response_require(const char *description, const char *value)
{
	if (strstr(response, value) == NULL) {
		LOG_ERR("%s verification failed: %s", description, response);
		return -EIO;
	}

	return 0;
}

static int edrx_disable_and_verify(void)
{
	int ret = command_send("AT+CEDRXS=0");

	if (ret < 0) {
		return ret;
	}
	ret = command_send("AT+CEDRXS?");
	if (ret < 0) {
		return ret;
	}
	if (strstr(response, "+CEDRXS:") != NULL) {
		LOG_ERR("eDRX remains configured: %s", response);
		return -EIO;
	}

	return 0;
}

static int profile_configure(void)
{
	int ret = command_send("AT+CFUN=4");

	if (ret < 0) {
		return ret;
	}

#if defined(CONFIG_HOST_POWER_PROFILE_REGISTERED) || defined(CONFIG_HOST_POWER_PROFILE_XSLEEP)
	ret = command_send("AT+CPSMS=0");
	if (ret < 0) {
		return ret;
	}
	ret = edrx_disable_and_verify();
	if (ret < 0) {
		return ret;
	}
	ret = command_send("AT+CPSMS?");
	if (ret < 0) {
		return ret;
	}
	ret = response_require("PSM disabled", "+CPSMS: 0");
	if (ret < 0) {
		return ret;
	}

#if defined(CONFIG_HOST_POWER_PROFILE_XSLEEP)
	LOG_INF("Host power profile: host-issued XSLEEP between cycles; "
		"PSM=off; eDRX=off; verified");
#else
	LOG_INF("Host power profile: registered; PSM=off; eDRX=off; verified");
#endif
#else
	char command[64];

	snprintk(command, sizeof(command), "AT+CPSMS=1,,,\"%s\",\"%s\"",
		 CONFIG_HOST_POWER_PROFILE_PSM_TAU,
		 CONFIG_HOST_POWER_PROFILE_PSM_ACTIVE_TIME);
	ret = command_send(command);
	if (ret < 0) {
		return ret;
	}
	ret = edrx_disable_and_verify();
	if (ret < 0) {
		return ret;
	}
	ret = command_send("AT+CPSMS?");
	if (ret < 0) {
		return ret;
	}
	ret = response_require("PSM enabled", "+CPSMS: 1");
	if (ret < 0) {
		return ret;
	}
	ret = response_require("requested periodic TAU",
			       CONFIG_HOST_POWER_PROFILE_PSM_TAU);
	if (ret < 0) {
		return ret;
	}
	ret = response_require("requested Active-Time",
			       CONFIG_HOST_POWER_PROFILE_PSM_ACTIVE_TIME);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("Host power profile: PSM=requested; eDRX=off; "
		"TAU=%s; Active-Time=%s; verified",
		CONFIG_HOST_POWER_PROFILE_PSM_TAU,
		CONFIG_HOST_POWER_PROFILE_PSM_ACTIVE_TIME);
#endif

	return 0;
}

static int host_power_profile_init(void)
{
	int ret;
	int uninit_ret;

	/* Run immediately before the cellular device (POST_KERNEL priority 99).
	 * This lets the AT client use UART temporarily, after which the cellular
	 * driver installs its own UART callback during device initialization. */
	k_sleep(K_SECONDS(MODEM_BOOT_DELAY_SECONDS));

	ret = sm_at_client_init(response_handler, false, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("Cannot initialize temporary AT client: %d", ret);
		profile_result = ret;
		return ret;
	}

	ret = command_send("AT");
	if (ret == 0) {
		ret = profile_configure();
	}

	uninit_ret = sm_at_client_uninit();
	if (uninit_ret < 0) {
		LOG_ERR("Cannot release temporary AT client: %d", uninit_ret);
		if (ret == 0) {
			ret = uninit_ret;
		}
	}

	/* Let the UART and DTR transition finish before the cellular driver takes
	 * ownership and starts its normal AT/CMUX/PPP sequence. */
	k_sleep(K_MSEC(100));
	profile_result = ret;
	return ret;
}

SYS_INIT(host_power_profile_init, POST_KERNEL, 98);

int host_power_profile_apply(void)
{
	return profile_result;
}

bool host_power_profile_suspends_between_cycles(void)
{
	return IS_ENABLED(CONFIG_HOST_POWER_PROFILE_XSLEEP);
}

#else

int host_power_profile_apply(void)
{
	LOG_INF("Host power profile: unchanged");
	return 0;
}

bool host_power_profile_suspends_between_cycles(void)
{
	return false;
}

#endif
