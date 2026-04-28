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
    phoneapi_my_info_t          my_info;
    phoneapi_metadata_t         metadata;
    phoneapi_channel_t          channels[PHONEAPI_CHANNEL_COUNT];
    phoneapi_node_t             nodes[PHONEAPI_NODES_CAP];
    phoneapi_config_device_t    config_device;
    phoneapi_config_lora_t      config_lora;
    phoneapi_config_position_t  config_position;
    phoneapi_config_display_t   config_display;
    bool                        my_info_valid;
    bool                        metadata_valid;
    bool                        config_device_valid;
    bool                        config_lora_valid;
    bool                        config_position_valid;
    bool                        config_display_valid;

    uint32_t change_seq;        // bump on every write
    uint32_t committed_seq;     // bump on phoneapi_cache_commit()
    uint32_t current_phase_seq; // bump on phoneapi_cache_phase_begin()
    bool     config_complete;
} s_cache;

static SemaphoreHandle_t s_lock = NULL;

/* SWD debug shadow — fixed-layout scratch that mirrors validity flags
 * and a few representative field values from s_cache. Updated inside
 * each set_config_*. Easy to read via mem32 without needing struct
 * offsets / DWARF info. NOT used by any code path. */
typedef struct {
    uint32_t magic;            /* 'B3P1' = 0x42335031 */
    uint8_t  config_device_valid;
    uint8_t  config_lora_valid;
    uint8_t  config_position_valid;
    uint8_t  config_display_valid;
    uint8_t  device_role;
    uint8_t  device_rebroadcast_mode;
    uint8_t  lora_region;
    uint8_t  position_gps_mode;
    uint8_t  display_oled;
    uint8_t  display_use_12h_clock;
    uint8_t  device_led_heartbeat_disabled;
    uint8_t  lora_use_preset;
    uint32_t lora_bandwidth;
    uint32_t position_position_flags;
} phoneapi_dbg_view_t;

phoneapi_dbg_view_t g_phoneapi_dbg = { .magic = 0x42335031u };

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

// ── Config sub-oneof writers / readers (B3-P1 / Cut B) ──────────────

void phoneapi_cache_set_config_device(const phoneapi_config_device_t *cfg)
{
    if (cfg == NULL) return;
    cache_lock();
    s_cache.config_device       = *cfg;
    s_cache.config_device_valid = true;
    s_cache.change_seq++;
    cache_unlock();
    g_phoneapi_dbg.config_device_valid          = 1u;
    g_phoneapi_dbg.device_role                  = cfg->role;
    g_phoneapi_dbg.device_rebroadcast_mode      = cfg->rebroadcast_mode;
    g_phoneapi_dbg.device_led_heartbeat_disabled = cfg->led_heartbeat_disabled ? 1u : 0u;
}

void phoneapi_cache_set_config_lora(const phoneapi_config_lora_t *cfg)
{
    if (cfg == NULL) return;
    cache_lock();
    s_cache.config_lora       = *cfg;
    s_cache.config_lora_valid = true;
    s_cache.change_seq++;
    cache_unlock();
    g_phoneapi_dbg.config_lora_valid = 1u;
    g_phoneapi_dbg.lora_region       = cfg->region;
    g_phoneapi_dbg.lora_use_preset   = cfg->use_preset ? 1u : 0u;
    g_phoneapi_dbg.lora_bandwidth    = cfg->bandwidth;
}

void phoneapi_cache_set_config_position(const phoneapi_config_position_t *cfg)
{
    if (cfg == NULL) return;
    cache_lock();
    s_cache.config_position       = *cfg;
    s_cache.config_position_valid = true;
    s_cache.change_seq++;
    cache_unlock();
    g_phoneapi_dbg.config_position_valid     = 1u;
    g_phoneapi_dbg.position_gps_mode         = cfg->gps_mode;
    g_phoneapi_dbg.position_position_flags   = cfg->position_flags;
}

void phoneapi_cache_set_config_display(const phoneapi_config_display_t *cfg)
{
    if (cfg == NULL) return;
    cache_lock();
    s_cache.config_display       = *cfg;
    s_cache.config_display_valid = true;
    s_cache.change_seq++;
    cache_unlock();
    g_phoneapi_dbg.config_display_valid  = 1u;
    g_phoneapi_dbg.display_oled          = cfg->oled;
    g_phoneapi_dbg.display_use_12h_clock = cfg->use_12h_clock ? 1u : 0u;
}

bool phoneapi_cache_get_config_device(phoneapi_config_device_t *out)
{
    cache_lock();
    bool ok = s_cache.config_device_valid;
    if (ok && out != NULL) *out = s_cache.config_device;
    cache_unlock();
    return ok;
}

bool phoneapi_cache_get_config_lora(phoneapi_config_lora_t *out)
{
    cache_lock();
    bool ok = s_cache.config_lora_valid;
    if (ok && out != NULL) *out = s_cache.config_lora;
    cache_unlock();
    return ok;
}

bool phoneapi_cache_get_config_position(phoneapi_config_position_t *out)
{
    cache_lock();
    bool ok = s_cache.config_position_valid;
    if (ok && out != NULL) *out = s_cache.config_position;
    cache_unlock();
    return ok;
}

bool phoneapi_cache_get_config_display(phoneapi_config_display_t *out)
{
    cache_lock();
    bool ok = s_cache.config_display_valid;
    if (ok && out != NULL) *out = s_cache.config_display;
    cache_unlock();
    return ok;
}

uint32_t phoneapi_cache_change_seq(void)    { return s_cache.change_seq; }
uint32_t phoneapi_cache_committed_seq(void) { return s_cache.committed_seq; }
bool     phoneapi_cache_config_complete(void) { return s_cache.config_complete; }

// ── Messages ring ───────────────────────────────────────────────────
//
// Single-producer (`phoneapi_session` decoder) / single-consumer
// (`messages_view` LVGL refresh). Slot writes happen under a separate
// mutex from the main cache so a long NodeDB upsert burst doesn't
// stall an inbound text. `seq` is bumped last with __ATOMIC_RELEASE so
// a consumer polling `latest_seq` sees the slot fully published.

static struct {
    phoneapi_text_msg_t entries[PHONEAPI_MSG_RING_CAP];
    uint32_t            next_seq;     // monotonic, 0 = "never published"
    uint32_t            count;        // 0..PHONEAPI_MSG_RING_CAP
    uint32_t            head;         // index of newest entry
} s_msgs;

static SemaphoreHandle_t s_msgs_lock = NULL;

static void msgs_lock_init_lazy(void)
{
    if (s_msgs_lock == NULL) {
        s_msgs_lock = xSemaphoreCreateMutex();
    }
}

void phoneapi_msgs_publish(uint32_t from_node_id,
                            uint32_t to_node_id,
                            uint8_t  channel_index,
                            const uint8_t *text,
                            uint16_t text_len)
{
    msgs_lock_init_lazy();
    if (text_len > PHONEAPI_MSG_TEXT_MAX) {
        text_len = PHONEAPI_MSG_TEXT_MAX;
    }
    if (s_msgs_lock != NULL) {
        xSemaphoreTake(s_msgs_lock, portMAX_DELAY);
    }

    uint32_t slot = (s_msgs.head + 1u) % PHONEAPI_MSG_RING_CAP;
    if (s_msgs.count == 0u) {
        slot = 0;  // very first write
    }
    phoneapi_text_msg_t *e = &s_msgs.entries[slot];
    e->from_node_id  = from_node_id;
    e->to_node_id    = to_node_id;
    e->channel_index = channel_index;
    e->text_len      = text_len;
    if (text_len > 0u && text != NULL) {
        memcpy(e->text, text, text_len);
    }
    e->seq = ++s_msgs.next_seq;

    s_msgs.head = slot;
    if (s_msgs.count < PHONEAPI_MSG_RING_CAP) {
        s_msgs.count++;
    }

    if (s_msgs_lock != NULL) {
        xSemaphoreGive(s_msgs_lock);
    }
}

uint32_t phoneapi_msgs_count(void)       { return s_msgs.count; }
uint32_t phoneapi_msgs_latest_seq(void)  { return s_msgs.next_seq; }

bool phoneapi_msgs_take_at_offset(uint32_t offset, phoneapi_text_msg_t *out)
{
    msgs_lock_init_lazy();
    bool ok = false;
    if (s_msgs_lock != NULL) {
        xSemaphoreTake(s_msgs_lock, portMAX_DELAY);
    }
    if (offset < s_msgs.count) {
        // newest is at head; offset N steps back wraps within ring
        uint32_t idx = (s_msgs.head + PHONEAPI_MSG_RING_CAP - offset) %
                       PHONEAPI_MSG_RING_CAP;
        *out = s_msgs.entries[idx];
        ok   = true;
    }
    if (s_msgs_lock != NULL) {
        xSemaphoreGive(s_msgs_lock);
    }
    return ok;
}
