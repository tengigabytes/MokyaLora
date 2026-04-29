/* canned_messages.h — A-4 canned (preset) message store.
 *
 * Read-only catalogue of short preset messages used by the A-4 picker
 * for one-key fast-send on the 5-way keyboard. v1 ships a hardcoded
 * default list (see canned_messages.c); v1.5 will pull the runtime list
 * via cascade AdminMessage `get_canned_message_module_messages` so the
 * user can edit it from the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_UI_CANNED_MESSAGES_H
#define MOKYA_CORE1_UI_CANNED_MESSAGES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CANNED_MAX_LEN  32u

/* Number of canned messages currently available. */
uint8_t canned_count(void);

/* Returns a NUL-terminated string for `idx` in [0, canned_count()).
 * Returns NULL for out-of-range. The pointer is valid until the next
 * canned-list mutation; v1 is read-only so it's effectively forever. */
const char *canned_at(uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_UI_CANNED_MESSAGES_H */
