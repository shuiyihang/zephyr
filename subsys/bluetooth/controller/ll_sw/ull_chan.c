/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <toolchain.h>
#include <sys/util.h>

#include "util/util.h"
#include "util/memq.h"
#include "util/mem.h"

#include "hal/ccm.h"

#include "pdu.h"

#include "lll.h"
#include "lll/lll_adv_types.h"
#include "lll_adv.h"
#include "lll/lll_adv_pdu.h"
#include "lll_conn.h"

#include "ull_adv_types.h"

#include "ull_adv_internal.h"
#include "ull_central_internal.h"

/* Initial channel map indicating Used and Unused data channels.
 * The HCI LE Set Host Channel Classification command allows the Host to
 * specify a channel classification for the data, secondary advertising,
 * periodic, and isochronous physical channels based on its local information.
 */
static uint8_t map[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
static uint8_t count = 37U;

static void chan_map_set(uint8_t const *const chan_map);

uint8_t ll_chm_update(uint8_t const *const chm)
{
	chan_map_set(chm);

#if defined(CONFIG_BT_CENTRAL)
	(void)ull_central_chm_update();
#endif /* CONFIG_BT_CENTRAL */

#if defined(CONFIG_BT_CTLR_ADV_PERIODIC)
	(void)ull_adv_sync_chm_update();
#endif /* CONFIG_BT_CTLR_ADV_PERIODIC */

	/* TODO: Should failure due to Channel Map Update being already in
	 *       progress be returned to caller?
	 */
	return 0;
}

int ull_chan_reset(void)
{
	/* initialise connection channel map */
	map[0] = 0xFF;
	map[1] = 0xFF;
	map[2] = 0xFF;
	map[3] = 0xFF;
	map[4] = 0x1F;
	count = 37U;

	return 0;
}

uint8_t ull_chan_map_get(uint8_t *const chan_map)
{
	(void)memcpy(chan_map, map, sizeof(map));

	return count;
}

static void chan_map_set(uint8_t const *const chan_map)
{
	(void)memcpy(map, chan_map, sizeof(map));
	count = util_ones_count_get(map, sizeof(map));
}
