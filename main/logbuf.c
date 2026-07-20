#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

// No PSRAM on this board; keep this modest. ~6 KB holds roughly the last
// couple hundred log lines, plenty to see a pairing/connect attempt.
#define LOGBUF_CAP 6144

static char s_ring[LOGBUF_CAP];
static uint32_t s_total_written;  // monotonic count of bytes ever written
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;

static void logbuf_write(const char *data, size_t len)
{
    if (len > LOGBUF_CAP) {
        // Longer than the whole ring: only the tail could survive anyway.
        data += len - LOGBUF_CAP;
        len = LOGBUF_CAP;
    }
    portENTER_CRITICAL(&s_lock);
    size_t pos = s_total_written % LOGBUF_CAP;
    size_t first = LOGBUF_CAP - pos;
    if (first > len) {
        first = len;
    }
    memcpy(s_ring + pos, data, first);
    if (len > first) {
        memcpy(s_ring, data + first, len - first);
    }
    s_total_written += (uint32_t)len;
    portEXIT_CRITICAL(&s_lock);
}

static int logbuf_vprintf(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);
    int ret = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);

    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);
    if (n > 0) {
        logbuf_write(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    return ret;
}

void logbuf_init(void)
{
    s_prev_vprintf = esp_log_set_vprintf(logbuf_vprintf);
}

uint32_t logbuf_head(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t head = s_total_written;
    portEXIT_CRITICAL(&s_lock);
    return head;
}

size_t logbuf_read(uint32_t after, char *out, size_t out_size,
                    uint32_t *next_pos, bool *dropped)
{
    if (out_size == 0) {
        if (next_pos) *next_pos = after;
        if (dropped) *dropped = false;
        return 0;
    }

    portENTER_CRITICAL(&s_lock);
    uint32_t total = s_total_written;
    uint32_t oldest = (total > LOGBUF_CAP) ? total - LOGBUF_CAP : 0;
    uint32_t start = after;
    bool lost = false;
    if (start < oldest || start > total) {
        start = oldest;
        lost = true;
    }
    size_t avail = total - start;
    size_t copy_len = avail < out_size - 1 ? avail : out_size - 1;
    size_t pos = start % LOGBUF_CAP;
    size_t first = LOGBUF_CAP - pos;
    if (first > copy_len) {
        first = copy_len;
    }
    memcpy(out, s_ring + pos, first);
    if (copy_len > first) {
        memcpy(out + first, s_ring, copy_len - first);
    }
    portEXIT_CRITICAL(&s_lock);

    out[copy_len] = '\0';
    if (next_pos) *next_pos = start + (uint32_t)copy_len;
    if (dropped) *dropped = lost;
    return copy_len;
}
