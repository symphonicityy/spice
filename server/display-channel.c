/*
   Copyright (C) 2009-2015 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "display-channel.h"

DisplayChannelClient *dcc_new(DisplayChannel *display,
                              RedClient *client, RedsStream *stream,
                              int mig_target,
                              uint32_t *common_caps, int num_common_caps,
                              uint32_t *caps, int num_caps)
{
    DisplayChannelClient *dcc;

    dcc = (DisplayChannelClient*)common_channel_new_client(
        (CommonChannel *)display, sizeof(DisplayChannelClient),
        client, stream, mig_target, TRUE,
        common_caps, num_common_caps,
        caps, num_caps);
    spice_return_val_if_fail(dcc, NULL);

    ring_init(&dcc->palette_cache_lru);
    dcc->palette_cache_available = CLIENT_PALETTE_CACHE_SIZE;

    return dcc;
}

MonitorsConfig* monitors_config_ref(MonitorsConfig *monitors_config)
{
    monitors_config->refs++;

    return monitors_config;
}

void monitors_config_unref(MonitorsConfig *monitors_config)
{
    if (!monitors_config) {
        return;
    }
    if (--monitors_config->refs != 0) {
        return;
    }

    spice_debug("freeing monitors config");
    free(monitors_config);
}

static void monitors_config_debug(MonitorsConfig *mc)
{
    int i;

    spice_debug("monitors config count:%d max:%d", mc->count, mc->max_allowed);
    for (i = 0; i < mc->count; i++)
        spice_debug("+%d+%d %dx%d",
                    mc->heads[i].x, mc->heads[i].y,
                    mc->heads[i].width, mc->heads[i].height);
}

MonitorsConfig* monitors_config_new(QXLHead *heads, ssize_t nheads, ssize_t max)
{
    MonitorsConfig *mc;

    mc = spice_malloc(sizeof(MonitorsConfig) + nheads * sizeof(QXLHead));
    mc->refs = 1;
    mc->count = nheads;
    mc->max_allowed = max;
    memcpy(mc->heads, heads, nheads * sizeof(QXLHead));
    monitors_config_debug(mc);

    return mc;
}
