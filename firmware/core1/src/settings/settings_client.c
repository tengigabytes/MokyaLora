/* settings_client.c — see settings_client.h. */

#include "settings_client.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "ipc_protocol.h"
#include "ipc_shared_layout.h"
#include "ipc_ringbuf.h"

#define REPLY_QUEUE_DEPTH  8u

static QueueHandle_t s_reply_q;
static uint8_t       s_seq;

void settings_client_init(void)
{
    if (s_reply_q == NULL) {
        s_reply_q = xQueueCreate(REPLY_QUEUE_DEPTH,
                                 sizeof(settings_client_reply_t));
    }
}

/* ── Senders ─────────────────────────────────────────────────────────── */

bool settings_client_send_get(uint16_t ipc_key, uint8_t channel_index)
{
    IpcPayloadGetConfig p;
    memset(&p, 0, sizeof(p));
    p.key           = ipc_key;
    p.channel_index = channel_index;
    return ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                         g_ipc_shared.c1_to_c0_slots,
                         IPC_RING_SLOT_COUNT,
                         IPC_CMD_GET_CONFIG,
                         s_seq++,
                         &p,
                         (uint16_t)sizeof(p));
}

bool settings_client_send_set(uint16_t ipc_key, uint8_t channel_index,
                              const void *value, uint16_t value_len)
{
    if (value_len > SETTINGS_CLIENT_VALUE_MAX) value_len = SETTINGS_CLIENT_VALUE_MAX;

    uint8_t buf[sizeof(IpcPayloadConfigValue) + SETTINGS_CLIENT_VALUE_MAX];
    memset(buf, 0, sizeof(IpcPayloadConfigValue));
    IpcPayloadConfigValue *cv = (IpcPayloadConfigValue *)buf;
    cv->key           = ipc_key;
    cv->value_len     = value_len;
    cv->channel_index = channel_index;
    if (value_len > 0 && value != NULL) {
        memcpy(cv->value, value, value_len);
    }
    return ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                         g_ipc_shared.c1_to_c0_slots,
                         IPC_RING_SLOT_COUNT,
                         IPC_CMD_SET_CONFIG,
                         s_seq++,
                         buf,
                         (uint16_t)(sizeof(IpcPayloadConfigValue) + value_len));
}

bool settings_client_send_commit(bool reboot)
{
    uint8_t cmd = reboot ? IPC_CMD_COMMIT_REBOOT : IPC_CMD_COMMIT_CONFIG;
    return ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                         g_ipc_shared.c1_to_c0_slots,
                         IPC_RING_SLOT_COUNT,
                         cmd,
                         s_seq++,
                         NULL,
                         0u);
}

/* ── Reply intake (from bridge_task) ─────────────────────────────────── */

void settings_client_dispatch_reply(uint8_t msg_id,
                                    const uint8_t *payload,
                                    uint16_t payload_len)
{
    if (s_reply_q == NULL) return;

    settings_client_reply_t r;
    memset(&r, 0, sizeof(r));

    if (msg_id == IPC_MSG_CONFIG_VALUE &&
        payload_len >= sizeof(IpcPayloadConfigValue)) {
        const IpcPayloadConfigValue *cv =
            (const IpcPayloadConfigValue *)payload;
        uint16_t header_size = (uint16_t)offsetof(IpcPayloadConfigValue, value);
        uint16_t in_payload =
            (payload_len > header_size) ? (uint16_t)(payload_len - header_size) : 0u;
        uint16_t vlen = cv->value_len;
        if (vlen > in_payload) vlen = in_payload;
        if (vlen > SETTINGS_CLIENT_VALUE_MAX) vlen = SETTINGS_CLIENT_VALUE_MAX;
        r.kind      = (uint8_t)SCR_VALUE;
        r.key       = cv->key;
        r.value_len = vlen;
        if (vlen > 0) memcpy(r.value, cv->value, vlen);
    } else if (msg_id == IPC_MSG_CONFIG_RESULT &&
               payload_len >= sizeof(IpcPayloadConfigResult)) {
        const IpcPayloadConfigResult *cr =
            (const IpcPayloadConfigResult *)payload;
        r.key = cr->key;
        switch (cr->result) {
        case 0: r.kind = (uint8_t)SCR_OK; break;
        case 1: r.kind = (uint8_t)SCR_UNKNOWN_KEY; break;
        case 2: r.kind = (uint8_t)SCR_INVALID_VALUE; break;
        case 3: r.kind = (uint8_t)SCR_BUSY; break;
        default: return;
        }
    } else {
        return;
    }

    /* Non-blocking — if the consumer falls behind, drop the oldest
     * by reading one out (settings_view is the only consumer and runs
     * at LVGL tick rate, so this is mostly defensive). */
    if (xQueueSend(s_reply_q, &r, 0) != pdTRUE) {
        settings_client_reply_t drop;
        (void)xQueueReceive(s_reply_q, &drop, 0);
        (void)xQueueSend(s_reply_q, &r, 0);
    }
}

/* ── LVGL-side consumer ──────────────────────────────────────────────── */

bool settings_client_take_reply(settings_client_reply_t *out)
{
    if (s_reply_q == NULL || out == NULL) return false;
    return xQueueReceive(s_reply_q, out, 0) == pdTRUE;
}
