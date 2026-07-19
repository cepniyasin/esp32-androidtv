#include "proto_frame.h"

#include <string.h>

#include "pb_encode.h"

size_t frame_varint_encode(uint32_t value, uint8_t out[5])
{
    size_t n = 0;
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        out[n++] = byte | (value ? 0x80 : 0);
    } while (value);
    return n;
}

int frame_send(const pb_msgdesc_t *fields, const void *msg,
               frame_write_fn write_fn, void *ctx)
{
    // Payload is encoded at a fixed offset; the varint prefix is placed
    // directly before it so the frame goes out as one write (one TLS record).
    uint8_t buf[5 + FRAME_MAX_MSG];
    pb_ostream_t os = pb_ostream_from_buffer(buf + 5, FRAME_MAX_MSG);
    if (!pb_encode(&os, fields, msg)) {
        return FRAME_ERR_ENCODE;
    }
    uint8_t hdr[5];
    size_t hdr_len = frame_varint_encode((uint32_t)os.bytes_written, hdr);
    uint8_t *start = buf + 5 - hdr_len;
    memcpy(start, hdr, hdr_len);
    if (write_fn(ctx, start, hdr_len + os.bytes_written) != 0) {
        return FRAME_ERR_IO;
    }
    return 0;
}

static int read_full(frame_read_fn read_fn, void *ctx, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = read_fn(ctx, buf + got, len - got);
        if (r == 0) {
            return FRAME_ERR_CLOSED;
        }
        if (r < 0) {
            return FRAME_ERR_IO;
        }
        got += (size_t)r;
    }
    return 0;
}

int frame_recv(uint8_t *buf, size_t buf_size, frame_read_fn read_fn, void *ctx)
{
    uint32_t len = 0;
    for (int shift = 0;; shift += 7) {
        if (shift > 28) {
            return FRAME_ERR_VARINT;
        }
        uint8_t byte;
        int r = read_full(read_fn, ctx, &byte, 1);
        if (r != 0) {
            return r;
        }
        len |= (uint32_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            break;
        }
    }
    if (len > buf_size || len > FRAME_MAX_MSG) {
        return FRAME_ERR_TOOBIG;
    }
    int r = read_full(read_fn, ctx, buf, len);
    if (r != 0) {
        return r;
    }
    return (int)len;
}
