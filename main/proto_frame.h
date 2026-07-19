#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pb.h"

// Length-delimited protobuf framing (varint length prefix, then payload),
// as implemented by the reference base.py. Transport-agnostic via callbacks
// so it can be unit-tested on the host.

// Largest message we send or accept. Remote/pairing messages are small;
// this bounds stack buffers.
#define FRAME_MAX_MSG 512

#define FRAME_ERR_IO (-1)      // transport error
#define FRAME_ERR_ENCODE (-2)  // nanopb encode failed
#define FRAME_ERR_TOOBIG (-3)  // incoming length > buf/FRAME_MAX_MSG
#define FRAME_ERR_CLOSED (-4)  // clean close mid-frame or before one
#define FRAME_ERR_VARINT (-5)  // malformed length prefix

// Reads up to len bytes. Returns >0 bytes read (may be short), 0 on clean
// close, <0 on transport error.
typedef int (*frame_read_fn)(void *ctx, uint8_t *buf, size_t len);

// Writes exactly len bytes. Returns 0 on success, <0 on transport error.
typedef int (*frame_write_fn)(void *ctx, const uint8_t *buf, size_t len);

// Encodes value as a protobuf varint into out. Returns the byte count (1-5).
size_t frame_varint_encode(uint32_t value, uint8_t out[5]);

// Encodes msg and writes varint(len) ++ payload as a single write.
// Returns 0 on success or a FRAME_ERR_* code.
int frame_send(const pb_msgdesc_t *fields, const void *msg,
               frame_write_fn write_fn, void *ctx);

// Reads one full frame into buf, accumulating across partial reads.
// Returns the payload length (>= 0) or a FRAME_ERR_* code.
int frame_recv(uint8_t *buf, size_t buf_size, frame_read_fn read_fn, void *ctx);
