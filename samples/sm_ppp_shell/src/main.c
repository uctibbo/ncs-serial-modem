/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <errno.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ppp.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include "httpbin_ca.h"
#include "host_power_profile.h"

LOG_MODULE_REGISTER(https_test, LOG_LEVEL_INF);

#define HOST "httpbin.org"
#define CA_TAG 42
#define IO_TIMEOUT_MS 15000
#define SNTP_TIMEOUT_MS 5000
#define MODEM_BOOT_DELAY_SECONDS 2
#define PPP_START_TIMEOUT_SECONDS 180
#define PPP_RESTART_DELAY_SECONDS 2

struct request_result {
	size_t bytes;
	uint16_t status;
	bool complete;
};

static uint8_t response_buffer[1024];

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

static int synchronize_time(void)
{
	struct sntp_time now;
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM,
	};
	struct zsock_addrinfo *address = NULL;
	int ret = zsock_getaddrinfo("time.google.com", "123", &hints, &address);

	if (ret != 0) {
		return -EHOSTUNREACH;
	}
	ret = sntp_simple_addr(address->ai_addr, address->ai_addrlen,
			       SNTP_TIMEOUT_MS, &now);
	zsock_freeaddrinfo(address);

	if (ret < 0) {
		return ret;
	}
	struct timespec ts = {.tv_sec = now.seconds, .tv_nsec = 0};

	if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
		return -errno;
	}
	LOG_INF("Time synchronized; certificate date checking enabled");
	return 0;
}

static int request_once(struct request_result *result)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET, .ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *addresses = NULL;
	const sec_tag_t tags[] = {CA_TAG};
	const char *headers[] = {"Connection: close\r\n", "Accept: application/json\r\n", NULL};
	struct zsock_timeval timeout = {.tv_sec = IO_TIMEOUT_MS / 1000};
	int verify = TLS_PEER_VERIFY_REQUIRED;
	int ret = zsock_getaddrinfo(HOST, "443", &hints, &addresses);
	int fd = -1;

	if (ret != 0) {
		LOG_WRN("DNS lookup failed: %d", ret);
		return -EHOSTUNREACH;
	}
	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}
	if (zsock_setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags)) < 0 ||
		zsock_setsockopt(fd, SOL_TLS, TLS_HOSTNAME, HOST, sizeof(HOST) - 1) < 0 ||
		zsock_setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify)) < 0 ||
		zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
		zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		ret = -errno;
		goto out;
	}
	if (zsock_connect(fd, addresses->ai_addr, addresses->ai_addrlen) < 0) {
		ret = -errno;
		LOG_WRN("TCP/TLS connection failed: %d", ret);
		goto out;
	}
	struct http_request req = {
		.method = HTTP_GET, .url = "/get", .host = HOST,
		.protocol = "HTTP/1.1", .header_fields = headers,
		.response = response_cb, .recv_buf = response_buffer,
		.recv_buf_len = sizeof(response_buffer),
	};

	ret = http_client_req(fd, &req, IO_TIMEOUT_MS, result);
	if (ret >= 0) {
		ret = result->status == 200 && result->complete ? 0 : -EBADMSG;
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

static int sleep_until_next_cycle(struct net_if *iface, int64_t deadline)
{
	int ret = net_if_down(iface);

	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("Cannot suspend PPP for XSLEEP: %d", ret);
		return ret;
	}

	LOG_INF("PPP suspended; host shutdown script requested");
	while (true) {
		int64_t remaining = deadline - k_uptime_get();

		if (remaining <= 0) {
			break;
		}
		k_sleep(K_MSEC(MIN(remaining, 1000)));
	}

	LOG_INF("Sleep deadline reached; resuming PPP and waking Serial Modem with DTR");
	return start_ppp(iface);
}

static void https_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	const int64_t interval = (int64_t)CONFIG_HTTPS_TEST_INTERVAL_SECONDS * 1000;
	bool time_ready = false;
	const bool sleep_between_cycles =
		host_power_profile_suspends_between_cycles();
	uint32_t cycle = 0;
	int64_t next = 0;
	int64_t ppp_started;
	int ret;

	if (iface == NULL) {
		LOG_ERR("No PPP interface: check the board modem overlay");
		return;
	}
	ret = tls_credential_add(CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
							 httpbin_ca, sizeof(httpbin_ca));
	if (ret < 0) {
		LOG_ERR("Cannot install CA certificate: %d", ret);
		return;
	}
	net_if_set_default(iface);
	/* On simultaneous board power-up, let Serial Modem advertise Ready before
	 * the cellular driver sends its first AT command. */
	k_sleep(K_SECONDS(MODEM_BOOT_DELAY_SECONDS));
	ret = host_power_profile_apply();
	if (ret < 0) {
		LOG_ERR("Cannot apply host power profile: %d", ret);
		return;
	}
	ret = start_ppp(iface);
	if (ret < 0) {
		return;
	}
	ppp_started = k_uptime_get();
	LOG_INF("Waiting for PPP IPv4; GET https://" HOST "/get every %d seconds",
			CONFIG_HTTPS_TEST_INTERVAL_SECONDS);
	while (true) {
		if (!connected(iface)) {
			if (!sleep_between_cycles) {
				next = 0;
			}
			if (k_uptime_get() - ppp_started >=
			    PPP_START_TIMEOUT_SECONDS * MSEC_PER_SEC) {
				if (sleep_between_cycles) {
					LOG_WRN("PPP still has no IPv4 after %d seconds; "
						"XSLEEP profile continues waiting",
						PPP_START_TIMEOUT_SECONDS);
					ppp_started = k_uptime_get();
				} else {
					ret = restart_ppp(iface);
					if (ret < 0) {
						return;
					}
					ppp_started = k_uptime_get();
				}
			}
			k_sleep(K_SECONDS(1));
			continue;
		}
		/* A later disconnect gets a fresh recovery window. */
		ppp_started = k_uptime_get();
		if (!time_ready) {
			ret = synchronize_time();
			if (ret < 0) {
				LOG_WRN("Time sync failed: %d; retry in 10 seconds", ret);
				k_sleep(K_SECONDS(10));
				continue;
			}
			time_ready = true;
		}
		int64_t now = k_uptime_get();

		if (next == 0) {
			next = now;
		}
		if (now < next) {
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
		next += interval;
		now = k_uptime_get();
		if (next <= now) {
			int64_t skipped = (now - next) / interval + 1;

			next += skipped * interval;
			LOG_WRN("cycle=%u skipped_intervals=%lld", cycle, (long long)skipped);
		}
		if (sleep_between_cycles) {
			ret = sleep_until_next_cycle(iface, next);
			if (ret < 0) {
				return;
			}
			ppp_started = k_uptime_get();
		}
	}
}

K_THREAD_DEFINE(https_thread, 8192, https_worker, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	return 0;
}
