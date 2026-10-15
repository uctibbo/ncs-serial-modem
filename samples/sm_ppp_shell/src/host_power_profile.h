/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef HOST_POWER_PROFILE_H_
#define HOST_POWER_PROFILE_H_

#include <stdbool.h>
#include <zephyr/net/net_if.h>

int host_power_profile_apply(struct net_if *iface);
bool host_power_profile_suspends_between_cycles(void);
void host_power_profile_network_lock(void);
void host_power_profile_network_unlock(void);

#endif /* HOST_POWER_PROFILE_H_ */
