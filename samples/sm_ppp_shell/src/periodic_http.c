/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ppp.h>
#include <zephyr/net/socket.h>

#include "v2_power_profile.h"

LOG_MODULE_REGISTER(periodic_http, LOG_LEVEL_INF);

#define HTTP_HOST "httpbin.org"
#define HTTP_PATH "/bytes/1?seed=54"
#define HTTP_EXPECTED_BYTES 1
#define IO_TIMEOUT_MS 15000
#define MODEM_BOOT_DELAY_SECONDS 2
#define PPP_START_TIMEOUT_SECONDS 180
#define PPP_RESTART_DELAY_SECONDS 2

struct request_result {
	size_t bytes;
	uint16_t status;
	bool complete;
};

static uint8_t response_buffer[1024];
static const char *const request_headers[] = {
	"Connection: close\r\n",
	"Accept: application/octet-stream\r\n",
	NULL,
};

static bool connected(struct net_if *iface)
{
	return net_if_is_up(iface) && net_if_is_carrier_ok(iface) &&
	       net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

static int response_cb(struct http_response *rsp, enum http_final_call final,
		       void *user_data)
{
	struct request_result *result = user_data;

	result->status = rsp->http_status_code;
	result->bytes += rsp->body_frag_len;
	result->complete = final == HTTP_DATA_FINAL && rsp->message_complete;
	return 0;
}

static int request_once(struct request_result *result)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *addresses = NULL;
	char address_string[NET_IPV4_ADDR_LEN];
	struct zsock_timeval timeout = {.tv_sec = IO_TIMEOUT_MS / 1000};
	int ret = zsock_getaddrinfo(HTTP_HOST, "80", &hints, &addresses);
	int fd = -1;

	if (ret != 0) {
		LOG_WRN("DNS lookup failed: %d", ret);
		return -EHOSTUNREACH;
	}

	if (zsock_inet_ntop(AF_INET,
			    &((struct sockaddr_in *)addresses->ai_addr)->sin_addr,
			    address_string, sizeof(address_string)) != NULL) {
		LOG_INF("Connecting to %s", address_string);
	}

	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}
	/* CONFIG_NET_CONTEXT_RCVTIMEO supports this option. Do not set
	 * SO_SNDTIMEO: unlike the TLS wrapper, a native TCP socket returns
	 * ENOPROTOOPT unless CONFIG_NET_CONTEXT_SNDTIMEO is enabled.
	 */
	if (zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		ret = -errno;
		LOG_WRN("Cannot set TCP receive timeout: %d", ret);
		goto out;
	}
	if (zsock_connect(fd, addresses->ai_addr, addresses->ai_addrlen) < 0) {
		ret = -errno;
		LOG_WRN("TCP connection failed: %d", ret);
		goto out;
	}

	struct http_request request = {
		.method = HTTP_GET,
		.url = HTTP_PATH,
		.host = HTTP_HOST,
		.protocol = "HTTP/1.1",
		.header_fields = request_headers,
		.response = response_cb,
		.recv_buf = response_buffer,
		.recv_buf_len = sizeof(response_buffer),
	};

	ret = http_client_req(fd, &request, IO_TIMEOUT_MS, result);
	if (ret >= 0) {
		ret = result->status == 200 && result->complete &&
		      result->bytes == HTTP_EXPECTED_BYTES ? 0 : -EBADMSG;
	}

out:
	if (fd >= 0) {
		zsock_close(fd);
	}
	zsock_freeaddrinfo(addresses);
	return ret;
}

static int start_ppp(struct net_if *iface)
{
	int ret = net_if_up(iface);

	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Cannot start PPP: %d", ret);
		return ret;
	}
	return 0;
}

static int restart_ppp(struct net_if *iface)
{
	int ret;

	LOG_WRN("PPP has no IPv4 after %d seconds; restarting interface",
		PPP_START_TIMEOUT_SECONDS);
	ret = net_if_down(iface);
	if (ret < 0 && ret != -EALREADY) {
		LOG_WRN("Cannot stop PPP for recovery: %d", ret);
	}
	k_sleep(K_SECONDS(PPP_RESTART_DELAY_SECONDS));
	return start_ppp(iface);
}

static int wait_for_initial_connection(struct net_if *iface)
{
	int64_t started = k_uptime_get();

	while (!connected(iface)) {
		if (k_uptime_get() - started >=
		    PPP_START_TIMEOUT_SECONDS * MSEC_PER_SEC) {
			int ret = restart_ppp(iface);

			if (ret < 0) {
				return ret;
			}
			started = k_uptime_get();
		}
		k_sleep(K_SECONDS(1));
	}
	return 0;
}

static void http_worker(void *a, void *b, void *c)
{
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	const int64_t interval = (int64_t)CONFIG_HTTP_TEST_INTERVAL_SECONDS * MSEC_PER_SEC;
	uint32_t cycle = 0;
	int64_t next = 0;
	int64_t disconnected_at = 0;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (iface == NULL) {
		LOG_ERR("No PPP interface: check the board modem overlay");
		return;
	}

	net_if_set_default(iface);
	/* Match the proven v2 host flow in OSS_ZEPHYR: allow Serial Modem to
	 * advertise Ready after simultaneous host/modem power-up.
	 */
	k_sleep(K_SECONDS(MODEM_BOOT_DELAY_SECONDS));
	ret = start_ppp(iface);
	if (ret < 0) {
		return;
	}
	LOG_INF("Waiting for initial PPP IPv4 before opening a v2 user pipe");
	ret = wait_for_initial_connection(iface);
	if (ret < 0) {
		return;
	}

	ret = v2_power_profile_apply_initial(iface);
	if (ret < 0) {
		LOG_ERR("Cannot apply initial v2 power profile: %d", ret);
		return;
	}
	v2_power_profile_report_network();

	LOG_INF("GET http://" HTTP_HOST HTTP_PATH " every %d seconds",
		CONFIG_HTTP_TEST_INTERVAL_SECONDS);
	while (true) {
		if (!connected(iface)) {
			next = 0;
			if (disconnected_at == 0) {
				disconnected_at = k_uptime_get();
			}
			if (k_uptime_get() - disconnected_at >=
			    PPP_START_TIMEOUT_SECONDS * MSEC_PER_SEC) {
				ret = restart_ppp(iface);
				if (ret < 0) {
					return;
				}
				disconnected_at = k_uptime_get();
			}
			k_sleep(K_SECONDS(1));
			continue;
		}
		disconnected_at = 0;

		v2_power_profile_network_lock();
		if (!connected(iface)) {
			v2_power_profile_network_unlock();
			continue;
		}

		int64_t now = k_uptime_get();

		if (next == 0) {
			next = now;
		}
		if (now < next) {
			v2_power_profile_network_unlock();
			k_sleep(K_MSEC(MIN(next - now, 1000)));
			continue;
		}

		struct request_result result = {0};
		int64_t start = k_uptime_get();

		LOG_INF("cycle=%u start", ++cycle);
		ret = request_once(&result);
		LOG_INF("cycle=%u %s status=%u bytes=%u complete=%u duration_ms=%lld error=%d",
			cycle, ret == 0 ? "SUCCESS" : "FAIL", result.status,
			(unsigned int)result.bytes, result.complete,
			(long long)(k_uptime_get() - start), ret);
		v2_power_profile_network_unlock();

		next += interval;
		now = k_uptime_get();
		if (next <= now) {
			int64_t skipped = (now - next) / interval + 1;

			next += skipped * interval;
			LOG_WRN("cycle=%u skipped_intervals=%lld", cycle,
				(long long)skipped);
		}
	}
}

K_THREAD_DEFINE(http_thread, 8192, http_worker, NULL, NULL, NULL, 7, 0, 0);
