/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef HOST_POWER_PROFILE_H_
#define HOST_POWER_PROFILE_H_

#include <stdbool.h>

int host_power_profile_apply(void);
bool host_power_profile_suspends_between_cycles(void);

#endif /* HOST_POWER_PROFILE_H_ */
