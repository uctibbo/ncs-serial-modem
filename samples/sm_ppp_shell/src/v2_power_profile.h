/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef V2_POWER_PROFILE_H_
#define V2_POWER_PROFILE_H_

#include <zephyr/net/net_if.h>

int v2_power_profile_apply_initial(struct net_if *iface);
void v2_power_profile_report_network(void);
void v2_power_profile_network_lock(void);
void v2_power_profile_network_unlock(void);

#endif /* V2_POWER_PROFILE_H_ */
