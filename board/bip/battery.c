/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Battery pack vendor provided charging profile
 */
#include "battery.h"
#include "battery_smart.h"
/* TODO(b/74353771): Once CL:978619 lands. Pull common code in baseboard. */

/* TODO(b/74353771): Ensure settings here are correct */
static const struct battery_info info = {
	.voltage_max = 13200, /* mV */
	.voltage_normal = 11550,
	.voltage_min = 9000,
	.precharge_current = 256, /* mA */
	.start_charging_min_c = 0,
	.start_charging_max_c = 50,
	.charging_min_c = 0,
	.charging_max_c = 60,
	.discharging_min_c = -20,
	.discharging_max_c = 75,
};
const struct battery_info *battery_get_info(void)
{
	return &info;
}
int board_cut_off_battery(void)
{
	/* TODO(b/74353771): Ensure settings here are correct */
	return EC_RES_ERROR;
}
