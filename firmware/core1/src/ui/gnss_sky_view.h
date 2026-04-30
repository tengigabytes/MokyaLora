/* gnss_sky_view.h — T-6 GNSS satellite sky view.
 *
 * Polar projection of teseo_get_sat_view() output.  Panel split:
 *
 *   Top half  (y   0..159, 160 px) — sky chart, 144 × 144 polar disc
 *                                    centred horizontally; concentric
 *                                    rings at elev 30° / 60° / 90°,
 *                                    cardinal labels (N/E/S/W).  Each
 *                                    satellite renders as a 14×14 px
 *                                    label "PRN" coloured by C/N0.
 *   Bottom half (y 161..223, 63 px) — 3-line summary: total in view,
 *                                    snr histogram, fix state.
 *
 * 1 Hz refresh — Teseo polls at 100 ms but the sat view only updates
 * once per talker cycle (~1 s per constellation), so a faster cadence
 * just wastes draws.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *gnss_sky_view_descriptor(void);

#ifdef __cplusplus
}
#endif
