/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ppp.h>
#include <zephyr/pm/device.h>
#include <zephyr/shell/shell.h>

#include <sm_at_client.h>

#include "host_power_profile.h"

LOG_MODULE_REGISTER(host_power_profile, LOG_LEVEL_INF);

#define COMMAND_TIMEOUT_SECONDS 10
#define HANDOFF_TIMEOUT_SECONDS 35
#define RESPONSE_BUFFER_SIZE 192

/* Supplied by patches/ncs-v3.2.1-nrf91-slm-uart-handoff.patch. */
extern int modem_cellular_uart_handoff_begin(const struct device *dev);
extern int modem_cellular_uart_handoff_end(const struct device *dev);

enum power_profile {
	POWER_PROFILE_UNCHANGED,
	POWER_PROFILE_REGISTERED,
	POWER_PROFILE_PSM,
	POWER_PROFILE_EDRX,
	POWER_PROFILE_PSM_EDRX,
};

static const struct device *const modem_dev = DEVICE_DT_GET(DT_NODELABEL(modem));
static K_SEM_DEFINE(response_complete, 0, 1);
static K_MUTEX_DEFINE(network_mutex);
static char response[RESPONSE_BUFFER_SIZE];
static size_t response_length;
static bool capture_response;
static enum power_profile active_profile = POWER_PROFILE_UNCHANGED;

static const char *profile_name(enum power_profile profile)
{
	switch (profile) {
	case POWER_PROFILE_REGISTERED:
		return "registered";
	case POWER_PROFILE_PSM:
		return "psm";
	case POWER_PROFILE_EDRX:
		return "edrx";
	case POWER_PROFILE_PSM_EDRX:
		return "psm_edrx";
	default:
		return "unchanged";
	}
}

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
	}

	return ret;
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

static int edrx_enable_and_verify(void)
{
	char command[40];
	int ret;

	snprintk(command, sizeof(command), "AT+CEDRXS=2,%d,\"%s\"",
		 CONFIG_HOST_POWER_PROFILE_EDRX_ACT_TYPE,
		 CONFIG_HOST_POWER_PROFILE_EDRX_VALUE);
	ret = command_send(command);
	if (ret < 0) {
		return ret;
	}
	ret = command_send("AT+CEDRXS?");
	if (ret < 0) {
		return ret;
	}
	ret = response_require("eDRX enabled", "+CEDRXS:");
	if (ret == 0) {
		ret = response_require("requested eDRX value",
				       CONFIG_HOST_POWER_PROFILE_EDRX_VALUE);
	}

	return ret;
}

static int profile_configure(enum power_profile profile)
{
	bool psm_enabled = profile == POWER_PROFILE_PSM ||
			   profile == POWER_PROFILE_PSM_EDRX;
	bool edrx_enabled = profile == POWER_PROFILE_EDRX ||
			    profile == POWER_PROFILE_PSM_EDRX;
	int ret = command_send("AT+CFUN=4");

	if (ret < 0) {
		return ret;
	}

	if (!psm_enabled) {
		ret = command_send("AT+CPSMS=0");
	} else {
		char command[64];

		snprintk(command, sizeof(command), "AT+CPSMS=1,,,\"%s\",\"%s\"",
			 CONFIG_HOST_POWER_PROFILE_PSM_TAU,
			 CONFIG_HOST_POWER_PROFILE_PSM_ACTIVE_TIME);
		ret = command_send(command);
	}
	if (ret < 0) {
		return ret;
	}

	ret = edrx_enabled ? edrx_enable_and_verify() : edrx_disable_and_verify();
	if (ret < 0) {
		return ret;
	}
	ret = command_send("AT+CPSMS?");
	if (ret < 0) {
		return ret;
	}

	if (!psm_enabled) {
		ret = response_require("PSM disabled", "+CPSMS: 0");
	} else {
		ret = response_require("PSM enabled", "+CPSMS: 1");
		if (ret == 0) {
			ret = response_require("requested periodic TAU",
					       CONFIG_HOST_POWER_PROFILE_PSM_TAU);
		}
		if (ret == 0) {
			ret = response_require("requested Active-Time",
					       CONFIG_HOST_POWER_PROFILE_PSM_ACTIVE_TIME);
		}
	}

	return ret;
}

static int wait_for_modem_suspended(void)
{
	int64_t deadline = k_uptime_get() + HANDOFF_TIMEOUT_SECONDS * MSEC_PER_SEC;

	while (k_uptime_get() < deadline) {
		enum pm_device_state state;
		int ret = pm_device_state_get(modem_dev, &state);

		if (ret < 0) {
			return ret;
		}
		if (state == PM_DEVICE_STATE_SUSPENDED) {
			return 0;
		}
		k_sleep(K_MSEC(20));
	}

	return -ETIMEDOUT;
}

static int profile_change(struct net_if *iface, enum power_profile profile, bool reconnect)
{
	bool handed_off = false;
	bool at_client_initialized = false;
	int first_error = 0;
	int ret;

	if (!device_is_ready(modem_dev)) {
		return -ENODEV;
	}

	k_mutex_lock(&network_mutex, K_FOREVER);

	ret = net_if_down(iface);
	if (ret < 0 && ret != -EALREADY) {
		first_error = ret;
		goto out;
	}

	ret = wait_for_modem_suspended();
	if (ret < 0) {
		LOG_ERR("Cellular driver did not suspend: %d", ret);
		first_error = ret;
		goto out;
	}

	ret = modem_cellular_uart_handoff_begin(modem_dev);
	if (ret < 0) {
		LOG_ERR("Cannot release cellular-driver UART backend: %d", ret);
		first_error = ret;
		goto out;
	}
	handed_off = true;

	/* Give Serial Modem time to finish leaving CMUX and return to raw AT. */
	k_sleep(K_MSEC(100));
	ret = sm_at_client_init(response_handler, false, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("Cannot initialize temporary AT client: %d", ret);
		first_error = ret;
		goto out;
	}
	at_client_initialized = true;

	ret = command_send("AT");
	if (ret == 0) {
		ret = profile_configure(profile);
	}
	if (ret < 0) {
		first_error = ret;
	} else {
		active_profile = profile;
		LOG_INF("Runtime power profile applied: %s", profile_name(profile));
	}

out:
	if (at_client_initialized) {
		ret = sm_at_client_uninit();
		if (ret < 0 && first_error == 0) {
			first_error = ret;
		}
	}
	if (handed_off) {
		ret = modem_cellular_uart_handoff_end(modem_dev);
		if (ret < 0) {
			LOG_ERR("Cannot restore cellular-driver UART backend: %d", ret);
			if (first_error == 0) {
				first_error = ret;
			}
			reconnect = false;
		}
	}

	/* Even a failed command may have reached CFUN=4. Reconnect after the UART
	 * backend is restored so the normal dial script returns to CFUN=1. */
	if (reconnect) {
		ret = net_if_up(iface);
		if (ret < 0 && ret != -EALREADY && first_error == 0) {
			first_error = ret;
		}
	}

	k_mutex_unlock(&network_mutex);
	return first_error;
}

int host_power_profile_apply(struct net_if *iface)
{
#if defined(CONFIG_HOST_POWER_PROFILE_REGISTERED) || defined(CONFIG_HOST_POWER_PROFILE_XSLEEP)
	return profile_change(iface, POWER_PROFILE_REGISTERED, false);
#elif defined(CONFIG_HOST_POWER_PROFILE_PSM)
	return profile_change(iface, POWER_PROFILE_PSM, false);
#else
	LOG_INF("Host power profile: unchanged; runtime command available");
	return 0;
#endif
}

bool host_power_profile_suspends_between_cycles(void)
{
	return IS_ENABLED(CONFIG_HOST_POWER_PROFILE_XSLEEP);
}

void host_power_profile_network_lock(void)
{
	k_mutex_lock(&network_mutex, K_FOREVER);
}

void host_power_profile_network_unlock(void)
{
	k_mutex_unlock(&network_mutex);
}

static int profile_shell(const struct shell *shell, size_t argc, char **argv)
{
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	enum power_profile profile;
	int ret;

	ARG_UNUSED(argc);

	if (iface == NULL) {
		shell_error(shell, "PPP interface not found");
		return -ENODEV;
	}
	if (strcmp(argv[1], "registered") == 0) {
		profile = POWER_PROFILE_REGISTERED;
	} else if (strcmp(argv[1], "psm") == 0) {
		profile = POWER_PROFILE_PSM;
	} else if (strcmp(argv[1], "edrx") == 0) {
		profile = POWER_PROFILE_EDRX;
	} else if (strcmp(argv[1], "psm_edrx") == 0) {
		profile = POWER_PROFILE_PSM_EDRX;
	} else if (strcmp(argv[1], "status") == 0) {
		shell_print(shell, "Requested profile: %s", profile_name(active_profile));
		return 0;
	} else {
		shell_error(shell, "Use registered, psm, edrx, psm_edrx, or status");
		return -EINVAL;
	}

	shell_print(shell, "Switching to %s; PPP will disconnect and reconnect",
		    profile_name(profile));
	ret = profile_change(iface, profile, true);
	if (ret < 0) {
		shell_error(shell, "Profile switch failed: %d", ret);
		return ret;
	}
	shell_print(shell, "Profile %s accepted; PPP restart initiated",
		    profile_name(profile));
	return 0;
}

SHELL_CMD_ARG_REGISTER(power_profile, NULL,
		       "Runtime profile: power_profile <registered|psm|edrx|psm_edrx|status>",
		       profile_shell, 2, 0);
