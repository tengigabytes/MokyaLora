/* wall_clock.c — see wall_clock.h. */

#include "wall_clock.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "c1_storage.h"
#include "../../lfs/lfs.h"

#define WALL_CLOCK_PATH         "/clock.bin"
#define WALL_CLOCK_MAGIC        0x434C434Bu      /* 'CLCK' */
#define WALL_CLOCK_VERSION      2u               /* v2: tz + gnss flag */
#define WALL_CLOCK_FLUSH_MS     (5u * 60u * 1000u)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t unix_secs;          /* last-known UTC wall time             */
    int16_t  tz_offset_min;      /* local = UTC + tz_offset_min*60       */
    uint8_t  gnss_sync_enable;   /* 1 = accept GNSS-derived time         */
    uint8_t  reserved[5];
} wc_record_t;                   /* 24 B */

/* SRAM-resident anchor — small, SWD-readable. */
volatile uint64_t g_wc_set_unix __attribute__((used)) = 0u;
volatile uint32_t g_wc_set_tick __attribute__((used)) = 0u;
volatile uint8_t  g_wc_synced   __attribute__((used)) = 0u;
volatile int16_t  g_wc_tz_offset_min __attribute__((used)) = 480;  /* default UTC+8 */
volatile uint8_t  g_wc_gnss_sync_enable __attribute__((used)) = 0u;

static wc_record_t s_record       __attribute__((section(".psram_bss")));
static TimerHandle_t s_flush_timer __attribute__((section(".psram_bss")));
static volatile bool s_inited     __attribute__((section(".psram_bss")));

/* SWD diag counters. */
volatile uint32_t g_wc_saves         __attribute__((used)) = 0u;
volatile uint32_t g_wc_loads         __attribute__((used)) = 0u;
volatile uint32_t g_wc_failures      __attribute__((used)) = 0u;
volatile uint32_t g_wc_gnss_applies  __attribute__((used)) = 0u;
volatile uint32_t g_wc_gnss_drops    __attribute__((used)) = 0u;

void wall_clock_set_unix(uint64_t unix_secs)
{
    g_wc_set_unix = unix_secs;
    g_wc_set_tick = xTaskGetTickCount();
    g_wc_synced   = 1u;
}

bool wall_clock_set_unix_from_gnss(uint64_t unix_secs)
{
    if (!g_wc_gnss_sync_enable) {
        g_wc_gnss_drops++;
        return false;
    }
    wall_clock_set_unix(unix_secs);
    g_wc_gnss_applies++;
    return true;
}

bool wall_clock_is_synced(void) { return g_wc_synced != 0u; }

uint64_t wall_clock_now_unix(void)
{
    if (!g_wc_synced) return 0u;
    uint32_t now_tick = xTaskGetTickCount();
    uint32_t delta_ticks = now_tick - g_wc_set_tick;     /* mod-2^32 */
    uint64_t delta_ms = (uint64_t)delta_ticks * portTICK_PERIOD_MS;
    return g_wc_set_unix + (delta_ms / 1000u);
}

uint64_t wall_clock_now_local(void)
{
    if (!g_wc_synced) return 0u;
    int64_t local = (int64_t)wall_clock_now_unix()
                  + (int64_t)g_wc_tz_offset_min * 60;
    if (local < 0) local = 0;
    return (uint64_t)local;
}

int16_t wall_clock_get_tz_offset_min(void)
{
    return g_wc_tz_offset_min;
}

void wall_clock_set_tz_offset_min(int16_t offset_min)
{
    /* Clamp to plausible range: -12 h .. +14 h. */
    if (offset_min < -720) offset_min = -720;
    if (offset_min >  840) offset_min =  840;
    g_wc_tz_offset_min = offset_min;
}

bool wall_clock_gnss_sync_is_enabled(void) { return g_wc_gnss_sync_enable != 0u; }
void wall_clock_gnss_sync_set_enabled(bool en) { g_wc_gnss_sync_enable = en ? 1u : 0u; }

uint16_t wall_clock_minute_of_day_from(uint64_t unix_secs,
                                       int16_t tz_offset_min)
{
    int64_t local = (int64_t)unix_secs + (int64_t)tz_offset_min * 60;
    if (local < 0) local = 0;
    uint32_t secs_of_day = (uint32_t)((uint64_t)local % 86400u);
    return (uint16_t)(secs_of_day / 60u);
}

uint16_t wall_clock_minute_of_day(void)
{
    if (!g_wc_synced) return 0xFFFFu;
    return wall_clock_minute_of_day_from(wall_clock_now_unix(),
                                          g_wc_tz_offset_min);
}

/* ── Civil-date conversion ────────────────────────────────────────── *
 *
 * Howard Hinnant's date algorithm — covers 1970..9999 exactly (no
 * leap seconds; consistent with Unix-epoch convention). */

static int days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int)doe - 719468;
}

static void civil_from_days(int z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    *y = (int)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp  = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10u ? 3u : -9u);
    *y += (*m <= 2u);
}

void wall_clock_unix_to_civil(uint64_t unix_secs, wall_clock_civil_t *out)
{
    if (out == NULL) return;
    int days = (int)(unix_secs / 86400u);
    uint32_t sod  = (uint32_t)(unix_secs % 86400u);
    int y; unsigned m; unsigned d;
    civil_from_days(days, &y, &m, &d);
    out->year     = (uint16_t)y;
    out->month    = (uint8_t)m;
    out->day      = (uint8_t)d;
    out->hour     = (uint8_t)(sod / 3600u);
    out->minute   = (uint8_t)((sod % 3600u) / 60u);
    out->second   = (uint8_t)(sod % 60u);
    out->reserved = 0;
}

uint64_t wall_clock_civil_to_unix(const wall_clock_civil_t *c)
{
    if (c == NULL) return 0;
    int days = days_from_civil((int)c->year, c->month, c->day);
    int64_t s = (int64_t)days * 86400
              + (int64_t)c->hour * 3600
              + (int64_t)c->minute * 60
              + (int64_t)c->second;
    if (s < 0) s = 0;
    return (uint64_t)s;
}

/* ── Persist ──────────────────────────────────────────────────────── */

bool wall_clock_flush_now(void)
{
    if (!c1_storage_is_mounted()) return false;

    s_record.magic            = WALL_CLOCK_MAGIC;
    s_record.version          = WALL_CLOCK_VERSION;
    s_record.unix_secs        = g_wc_synced ? wall_clock_now_unix() : 0u;
    s_record.tz_offset_min    = g_wc_tz_offset_min;
    s_record.gnss_sync_enable = g_wc_gnss_sync_enable;
    memset(s_record.reserved, 0, sizeof(s_record.reserved));

    c1_storage_file_t f;
    if (!c1_storage_open(&f, WALL_CLOCK_PATH,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        g_wc_failures++;
        return false;
    }
    int rc = c1_storage_write(&f, &s_record, sizeof(s_record));
    bool closed = c1_storage_close(&f);
    if (rc != (int)sizeof(s_record) || !closed) {
        g_wc_failures++;
        return false;
    }
    g_wc_saves++;
    return true;
}

static bool wall_clock_load(void)
{
    if (!c1_storage_is_mounted()) return false;
    if (!c1_storage_exists(WALL_CLOCK_PATH)) return false;

    c1_storage_file_t f;
    if (!c1_storage_open(&f, WALL_CLOCK_PATH, LFS_O_RDONLY)) {
        g_wc_failures++;
        return false;
    }
    int rc = c1_storage_read(&f, &s_record, sizeof(s_record));
    (void)c1_storage_close(&f);
    if (rc != (int)sizeof(s_record) ||
        s_record.magic   != WALL_CLOCK_MAGIC ||
        s_record.version != WALL_CLOCK_VERSION) {
        g_wc_failures++;
        return false;
    }
    /* Apply TZ + GNSS-flag first (no scheduler dependency), then the
     * unix value if non-zero. */
    g_wc_tz_offset_min     = s_record.tz_offset_min;
    g_wc_gnss_sync_enable  = s_record.gnss_sync_enable;
    if (s_record.unix_secs > 0u) {
        wall_clock_set_unix(s_record.unix_secs);
    }
    g_wc_loads++;
    return true;
}

static void flush_cb(TimerHandle_t t)
{
    (void)t;
    (void)wall_clock_flush_now();
}

void wall_clock_init(void)
{
    if (s_inited) return;
    s_inited = true;
    if (!c1_storage_is_mounted()) return;
    (void)wall_clock_load();
    s_flush_timer = xTimerCreate("wc_flush",
                                  pdMS_TO_TICKS(WALL_CLOCK_FLUSH_MS),
                                  pdTRUE, NULL, flush_cb);
    if (s_flush_timer != NULL) xTimerStart(s_flush_timer, 0);
}
