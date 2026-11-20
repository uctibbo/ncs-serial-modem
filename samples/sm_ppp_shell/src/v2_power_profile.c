/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/modem/modem_cellular.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/pipelink.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ppp.h>
#include <zephyr/shell/shell.h>

#include "v2_power_profile.h"

LOG_MODULE_REGISTER(v2_power_profile, LOG_LEVEL_INF);

#define USER_PIPE_WAIT_SECONDS 10
#define PPP_RETURN_TIMEOUT_SECONDS 180
#define RESPONSE_BUFFER_SIZE 512
#define CHAT_ARGV_COUNT 10

enum power_profile {
	POWER_PROFILE_UNCHANGED,
	POWER_PROFILE_REGISTERED,
	POWER_PROFILE_PSM,
	POWER_PROFILE_EDRX,
	POWER_PROFILE_PSM_EDRX,
};

struct profile_chat {
	struct modem_chat chat;
	uint8_t receive_buf[RESPONSE_BUFFER_SIZE];
	uint8_t *argv[CHAT_ARGV_COUNT];
	char response[RESPONSE_BUFFER_SIZE];
	size_t response_length;
};

static const struct device *const modem = DEVICE_DT_GET_ONE(nordic_nrf91_sm_v2);
static K_MUTEX_DEFINE(network_mutex);
static struct profile_chat profile_chat;
static enum power_profile active_profile = POWER_PROFILE_UNCHANGED;

static void response_line(struct modem_chat *chat, char **argv, uint16_t argc,
			  void *user_data);

MODEM_CELLULAR_COMMON_CHAT_MATCHES();
MODEM_CHAT_MATCH_WILDCARD_DEFINE(response_line_match, "???", "", response_line);

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

static bool connected(struct net_if *iface)
{
	return net_if_is_up(iface) && net_if_is_carrier_ok(iface) &&
	       net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

static void response_line(struct modem_chat *chat, char **argv, uint16_t argc,
			  void *user_data)
{
	struct profile_chat *instance = user_data;

	ARG_UNUSED(chat);

	for (uint16_t i = 0; i < argc; i++) {
		size_t length = strlen(argv[i]);
		size_t available = sizeof(instance->response) - 1 - instance->response_length;
		size_t copied = MIN(length, available);

		memcpy(instance->response + instance->response_length, argv[i], copied);
		instance->response_length += copied;
	}

	if (instance->response_length < sizeof(instance->response) - 1) {
		instance->response[instance->response_length++] = '\n';
	}
	instance->response[instance->response_length] = '\0';
}

static int chat_attach_user_pipe(struct profile_chat *instance)
{
	const struct modem_cellular_config *config = modem->config;
	const int64_t deadline = k_uptime_get() + USER_PIPE_WAIT_SECONDS * MSEC_PER_SEC;

	if (!device_is_ready(modem)) {
		return -ENODEV;
	}
	if (config->user_pipes_size == 0) {
		return -ENOTSUP;
	}

	const struct modem_chat_config chat_config = {
		.receive_buf = instance->receive_buf,
		.receive_buf_size = sizeof(instance->receive_buf),
		.delimiter = "\r",
		.delimiter_size = sizeof("\r") - 1,
		.filter = "\n",
		.filter_size = sizeof("\n") - 1,
		.argv = instance->argv,
		.argv_size = ARRAY_SIZE(instance->argv),
		.unsol_matches = &response_line_match,
		.unsol_matches_size = 1,
		.user_data = instance,
	};

	modem_chat_init(&instance->chat, &chat_config);

	while (k_uptime_get() < deadline) {
		/* The interactive AT shell normally takes DLCI 3. Prefer the other
		 * v2 user pipe so profile changes do not steal its attachment.
		 */
		for (int i = config->user_pipes_size - 1; i >= 0; i--) {
			struct modem_cellular_user_pipe *user_pipe = &config->user_pipes[i];
			struct modem_pipe *pipe = modem_pipelink_get_pipe(user_pipe->pipelink);

			if (!modem_pipelink_is_connected(user_pipe->pipelink) || pipe == NULL ||
			    pipe->callback != NULL) {
				continue;
			}

			int ret = modem_pipe_open(pipe, K_SECONDS(1));

			if (ret < 0) {
				continue;
			}
			modem_chat_attach(&instance->chat, pipe);
			LOG_INF("Using Serial Modem v2 user pipe DLCI %u",
				user_pipe->dlci_address);
			return 0;
		}
		k_sleep(K_MSEC(100));
	}

	return -EBUSY;
}

static int command_send(struct profile_chat *instance, const char *command)
{
	struct modem_chat_script_chat command_chat = {
		.request = (const uint8_t *)command,
		.request_size = strlen(command),
		.response_matches = &ok_match,
		.response_matches_size = 1,
		.timeout = 1000,
	};
	struct modem_chat_script script = {
		.name = "v2_power_profile_command",
		.script_chats = &command_chat,
		.script_chats_size = 1,
		.abort_matches = abort_matches,
		.abort_matches_size = ARRAY_SIZE(abort_matches),
		.timeout = 10,
	};
	int ret;

	instance->response_length = 0;
	instance->response[0] = '\0';
	ret = modem_chat_run_script(&instance->chat, &script);
	if (ret < 0) {
		LOG_ERR("%s failed: %d", command, ret);
		return ret;
	}

	if (instance->response_length > 0) {
		LOG_INF("%s response: %s", command, instance->response);
	}
	return 0;
}

static int response_require(struct profile_chat *instance, const char *description,
			    const char *value)
{
	if (strstr(instance->response, value) == NULL) {
		LOG_ERR("%s verification failed: %s", description, instance->response);
		return -EIO;
	}

	return 0;
}

static int edrx_configure(struct profile_chat *instance, bool enabled)
{
	char command[40];
	int ret;

	if (enabled) {
		snprintk(command, sizeof(command), "AT+CEDRXS=2,%d,\"%s\"",
			 CONFIG_HOST_POWER_PROFILE_EDRX_ACT_TYPE,
			 CONFIG_HOST_POWER_PROFILE_EDRX_VALUE);
		ret = command_send(instance, command);
	} else {
		ret = command_send(instance, "AT+CEDRXS=0");
	}
	if (ret < 0) {
		return ret;
	}

	ret = command_send(instance, "AT+CEDRXS?");
	if (ret < 0) {
		return ret;
	}
	if (!enabled) {
		if (strstr(instance->response, "+CEDRXS:") != NULL) {
			LOG_ERR("eDRX remains configured: %s", instance->response);
			return -EIO;
		}
		return 0;
	}

	ret = response_require(instance, "eDRX enabled", "+CEDRXS:");
	if (ret == 0) {
		ret = response_require(instance, "requested eDRX value",
				       CONFIG_HOST_POWER_PROFILE_EDRX_VALUE);
	}
	return ret;
}

static int profile_configure(struct profile_chat *instance, enum power_profile profile)
{
	const bool psm_enabled = profile == POWER_PROFILE_PSM ||
				 profile == POWER_PROFILE_PSM_EDRX;
	const bool edrx_enabled = profile == POWER_PROFILE_EDRX ||
				  profile == POWER_PROFILE_PSM_EDRX;
	char command[64];
	int ret;

	if (psm_enabled) {
		snprintk(command, sizeof(command), "AT+CPSMS=1,,,\"%s\",\"%s\"",
			 CONFIG_HOST_POWER_PROFILE_PSM_TAU,
			 CONFIG_HOST_POWER_PROFILE_PSM_ACTIVE_TIME);
		ret = command_send(instance, command);
	} else {
		ret = command_send(instance, "AT+CPSMS=0");
	}
	if (ret < 0) {
		return ret;
	}

	ret = edrx_configure(instance, edrx_enabled);
	if (ret < 0) {
		return ret;
	}

	ret = command_send(instance, "AT+CPSMS?");
	if (ret < 0) {
		return ret;
	}
	if (!psm_enabled) {
		return response_require(instance, "PSM disabled", "+CPSMS: 0");
	}

	ret = response_require(instance, "PSM enabled", "+CPSMS: 1");
	if (ret == 0) {
		ret = response_require(instance, "requested periodic TAU",
				       CONFIG_HOST_POWER_PROFILE_PSM_TAU);
	}
	if (ret == 0) {
		ret = response_require(instance, "requested Active-Time",
				       CONFIG_HOST_POWER_PROFILE_PSM_ACTIVE_TIME);
	}
	return ret;
}

static int wait_for_ppp(struct net_if *iface)
{
	const int64_t deadline = k_uptime_get() + PPP_RETURN_TIMEOUT_SECONDS * MSEC_PER_SEC;

	while (k_uptime_get() < deadline) {
		if (connected(iface)) {
			return 0;
		}
		k_sleep(K_SECONDS(1));
	}

	return -ETIMEDOUT;
}

static int profile_change(struct net_if *iface, enum power_profile profile)
{
	bool flight_mode = false;
	int first_error = 0;
	int ret;

	if (!connected(iface)) {
		return -ENETDOWN;
	}

	k_mutex_lock(&network_mutex, K_FOREVER);
	ret = chat_attach_user_pipe(&profile_chat);
	if (ret < 0) {
		LOG_ERR("No available Serial Modem v2 user pipe: %d", ret);
		first_error = ret;
		goto out;
	}

	ret = command_send(&profile_chat, "AT+CFUN=4");
	if (ret < 0) {
		first_error = ret;
		goto release;
	}
	flight_mode = true;

	ret = profile_configure(&profile_chat, profile);
	if (ret < 0) {
		first_error = ret;
	}

	/* Keep CMUX and its user pipe open. CFUN=1 lets the v2 driver observe
	 * registration again and resume the existing PPP interface.
	 */
	ret = command_send(&profile_chat, "AT+CFUN=1");
	if (ret < 0) {
		if (first_error == 0) {
			first_error = ret;
		}
	} else {
		flight_mode = false;
	}

release:
	if (flight_mode) {
		ret = command_send(&profile_chat, "AT+CFUN=1");
		if (ret < 0 && first_error == 0) {
			first_error = ret;
		}
	}
	modem_chat_release(&profile_chat.chat);

	if (first_error == 0) {
		ret = wait_for_ppp(iface);
		if (ret < 0) {
			LOG_ERR("PPP did not return after profile change: %d", ret);
			first_error = ret;
		} else {
			active_profile = profile;
			LOG_INF("Runtime power profile applied through v2 user pipe: %s",
				profile_name(profile));
		}
	}

out:
	k_mutex_unlock(&network_mutex);
	return first_error;
}

int v2_power_profile_apply_initial(struct net_if *iface)
{
#if defined(CONFIG_HOST_POWER_PROFILE_REGISTERED)
	return profile_change(iface, POWER_PROFILE_REGISTERED);
#elif defined(CONFIG_HOST_POWER_PROFILE_PSM)
	return profile_change(iface, POWER_PROFILE_PSM);
#elif defined(CONFIG_HOST_POWER_PROFILE_EDRX)
	return profile_change(iface, POWER_PROFILE_EDRX);
#elif defined(CONFIG_HOST_POWER_PROFILE_PSM_EDRX)
	return profile_change(iface, POWER_PROFILE_PSM_EDRX);
#else
	ARG_UNUSED(iface);
	LOG_INF("Initial power profile unchanged; runtime command available");
	return 0;
#endif
}

void v2_power_profile_report_network(void)
{
	int ret;

	k_mutex_lock(&network_mutex, K_FOREVER);
	ret = chat_attach_user_pipe(&profile_chat);
	if (ret < 0) {
		LOG_WRN("Cannot query network-granted power state: %d", ret);
		goto out;
	}

	/* %XMONITOR contains the network-provided PSM timers. CEDRXRDP reports
	 * the network-provided eDRX cycle and paging-time window. These queries
	 * are diagnostic and do not change the requested profile.
	 */
	(void)command_send(&profile_chat, "AT%XMONITOR");
	(void)command_send(&profile_chat, "AT+CEDRXRDP");
	modem_chat_release(&profile_chat.chat);

out:
	k_mutex_unlock(&network_mutex);
}

void v2_power_profile_network_lock(void)
{
	k_mutex_lock(&network_mutex, K_FOREVER);
}

void v2_power_profile_network_unlock(void)
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
	if (strcmp(argv[1], "status") == 0) {
		shell_print(shell, "Requested profile: %s", profile_name(active_profile));
		return 0;
	}
	if (strcmp(argv[1], "network") == 0) {
		v2_power_profile_report_network();
		return 0;
	}

	if (strcmp(argv[1], "registered") == 0) {
		profile = POWER_PROFILE_REGISTERED;
	} else if (strcmp(argv[1], "psm") == 0) {
		profile = POWER_PROFILE_PSM;
	} else if (strcmp(argv[1], "edrx") == 0) {
		profile = POWER_PROFILE_EDRX;
	} else if (strcmp(argv[1], "psm_edrx") == 0) {
		profile = POWER_PROFILE_PSM_EDRX;
	} else {
		shell_error(shell,
			    "Use registered, psm, edrx, psm_edrx, status, or network");
		return -EINVAL;
	}

	shell_print(shell, "Applying %s through a Serial Modem v2 CMUX user pipe",
		    profile_name(profile));
	ret = profile_change(iface, profile);
	if (ret < 0) {
		shell_error(shell, "Profile switch failed: %d", ret);
		return ret;
	}
	shell_print(shell, "Profile %s accepted; PPP is connected", profile_name(profile));
	return 0;
}

SHELL_CMD_ARG_REGISTER(
	power_profile, NULL,
	"Serial Modem v2 profile: <registered|psm|edrx|psm_edrx|status|network>",
	profile_shell, 2, 0);
