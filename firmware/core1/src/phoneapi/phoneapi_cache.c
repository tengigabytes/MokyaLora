// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 MokyaLora project contributors

#include "phoneapi_cache.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

// Cache state — exported through the API only. Everything is guarded
// by `s_lock`. SWD inspection still works via the static address.
static struct {
    phoneapi_my_info_t  my_info;
    phoneapi_metadata_t metadata;
    phoneapi_channel_t  channels[PHONEAPI_CHANNEL_COUNT];
    phoneapi_node_t     nodes[PHONEAPI_NODES_CAP];
    bool                my_info_valid;
    bool                metadata_valid;

    uint32_t change_seq;        // bump on every write
    uint32_t committed_seq;     // bump on phoneapi_cache_commit()
    uint32_t current_phase_seq; // bump on phoneapi_cache_phase_begin()
    bool     config_complete;
} s_cache;

static SemaphoreHandle_t s_lock = NULL;

static void cache_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void cache_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

void phoneapi_cache_init(void)
{
    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.current_phase_seq = 1;  // start at 1 so 0 means "never written"
    s_lock = xSemaphoreCreateMutex();
}

void phoneapi_cache_phase_begin(void)
{
    cache_lock();
    s_cache.current_phase_seq++;
    cache_unlock();
}

void phoneapi_cache_commit(uint32_t complete_id)
{
    (void)complete_id;
    cache_lock();
    // Evict NodeDB entries that did not receive a refresh during the
    // current phase. Entries from the steady-state (post-config) packet
    // stream get phase_seq == current_phase_seq via upsert, so they
    // survive too.
    for (size_t i = 0; i < PHONEAPI_NODES_CAP; i++) {
        if (s_cache.nodes[i].in_use &&
            s_cache.nodes[i].phase_seq != s_cache.current_phase_seq) {
            s_cache.nodes[i].in_use = false;
        }
    }
    s_cache.committed_seq++;
    s_cache.change_seq++;
    s_cache.config_complete = true;
    cache_unlock();
}

void phoneapi_cache_set_my_info(const phoneapi_my_info_t *info)
{
    cache_lock();
    s_cache.my_info       = *info;
    s_cache.my_info_valid = true;
    s_cache.change_seq++;
    cache_unlock();
}

void phoneapi_cache_set_metadata(const phoneapi_metadata_t *meta)
{
    cache_lock();
    s_cache.metadata       = *meta;
    s_cache.metadata_valid = true;
    s_cache.change_seq++;
    cache_unlock();
}

void phoneapi_cache_set_channel(uint8_t index, const phoneapi_channel_t *chan)
{
    if (index >= PHONEAPI_CHANNEL_COUNT) {
        return;
    }
    cache_lock();
    s_cache.channels[index]        = *chan;
    s_cache.channels[index].in_use = true;
    s_cache.channels[index].index  = index;
    s_cache.change_seq++;
    cache_unlock();
}

// Linear scan upsert. Find existing slot for this node_id, or take an
// empty slot, or evict the least-recently-touched (lowest phase_seq +
// not in current phase).
void phoneapi_cache_upsert_node(const phoneapi_node_t *node)
{
    if (node->num == 0u) {
        return;
    }
    cache_lock();

    int      hit_idx     = -1;
    int      empty_idx   = -1;
    int      victim_idx  = -1;
    uint32_t victim_seq  = UINT32_MAX;

    for (size_t i = 0; i < PHONEAPI_NODES_CAP; i++) {
        if (s_cache.nodes[i].in_use) {
            if (s_cache.nodes[i].num == node->num) {
                hit_idx = (int)i;
                break;
            }
            if (s_cache.nodes[i].phase_seq < victim_seq) {
                victim_seq = s_cache.nodes[i].phase_seq;
                victim_idx = (int)i;
            }
        } else if (empty_idx < 0) {
            empty_idx = (int)i;
        }
    }

    int slot = (hit_idx >= 0) ? hit_idx :
               (empty_idx >= 0) ? empty_idx :
               victim_idx;
    if (slot < 0) {
        // shouldn't happen: cap >= 1 always
        cache_unlock();
        return;
    }

    s_cache.nodes[slot]           = *node;
    s_cache.nodes[slot].in_use    = true;
    s_cache.nodes[slot].phase_seq = s_cache.current_phase_seq;
    s_cache.change_seq++;
    cache_unlock();
}

bool phoneapi_cache_get_my_info(phoneapi_my_info_t *out)
{
    cache_lock();
    bool ok = s_cache.my_info_valid;
    if (ok) {
        *out = s_cache.my_info;
    }
    cache_unlock();
    return ok;
}

bool phoneapi_cache_get_metadata(phoneapi_metadata_t *out)
{
    cache_lock();
    bool ok = s_cache.metadata_valid;
    if (ok) {
        *out = s_cache.metadata;
    }
    cache_unlock();
    return ok;
}

bool phoneapi_cache_get_channel(uint8_t index, phoneapi_channel_t *out)
{
    if (index >= PHONEAPI_CHANNEL_COUNT) {
        return false;
    }
    cache_lock();
    bool ok = s_cache.channels[index].in_use;
    if (ok) {
        *out = s_cache.channels[index];
    }
    cache_unlock();
    return ok;
}

uint32_t phoneapi_cache_node_count(void)
{
    cache_lock();
    uint32_t n = 0;
    for (size_t i = 0; i < PHONEAPI_NODES_CAP; i++) {
        if (s_cache.nodes[i].in_use) n++;
    }
    cache_unlock();
    return n;
}

bool phoneapi_cache_take_node_at(uint32_t index, phoneapi_node_t *out)
{
    cache_lock();
    uint32_t n = 0;
    bool     ok = false;
    for (size_t i = 0; i < PHONEAPI_NODES_CAP; i++) {
        if (s_cache.nodes[i].in_use) {
            if (n == index) {
                *out = s_cache.nodes[i];
                ok   = true;
                break;
            }
            n++;
        }
    }
    cache_unlock();
    return ok;
}

bool phoneapi_cache_get_node_by_id(uint32_t node_id, phoneapi_node_t *out)
{
    cache_lock();
    bool ok = false;
    for (size_t i = 0; i < PHONEAPI_NODES_CAP; i++) {
        if (s_cache.nodes[i].in_use && s_cache.nodes[i].num == node_id) {
            *out = s_cache.nodes[i];
            ok   = true;
            break;
        }
    }
    cache_unlock();
    return ok;
}

uint32_t phoneapi_cache_change_seq(void)    { return s_cache.change_seq; }
uint32_t phoneapi_cache_committed_seq(void) { return s_cache.committed_seq; }
bool     phoneapi_cache_config_complete(void) { return s_cache.config_complete; }
