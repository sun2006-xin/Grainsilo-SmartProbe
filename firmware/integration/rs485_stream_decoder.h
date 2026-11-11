/* Small adapter-side RS-485 stream decoder.
 *
 * AutoLink's Wire frame remains unchanged. UART reads are a byte stream, so
 * this helper extracts each length-delimited, CRC-validated frame even when
 * harmless noise precedes or follows it in the same UART burst.
 */
#ifndef GRAIN_SILO_RS485_STREAM_DECODER_H
#define GRAIN_SILO_RS485_STREAM_DECODER_H

#include <gs_proto.h>
#include <string.h>

static inline bool grainsilo_rs485_stream_take_frame(
    uint8_t *bytes, uint16_t *length, gs_frame_t *frame,
    uint16_t *discarded_prefix, int *last_parse_error)
{
    if (bytes == NULL || length == NULL || frame == NULL ||
        discarded_prefix == NULL || last_parse_error == NULL)
        return false;

    *discarded_prefix = 0u;
    *last_parse_error = 0;
    if (*length < 7u) return false;

    for (uint16_t start = 0u; start <= (uint16_t)(*length - 7u); ++start) {
        uint16_t frame_length;
        int parse_status;
        if (bytes[start] != GS_WIRE_VER) continue;

        frame_length = (uint16_t)7u + bytes[start + 4u];
        if (frame_length > GS_FRAME_MAX_LEN ||
            (uint32_t)start + frame_length > *length)
            continue;

        parse_status = gs_frame_parse(bytes + start, frame_length, frame);
        if (parse_status != GS_PARSE_OK) {
            *last_parse_error = parse_status;
            continue;
        }

        {
            const uint16_t consumed = (uint16_t)(start + frame_length);
            const uint16_t remaining = (uint16_t)(*length - consumed);
            if (remaining > 0u)
                memmove(bytes, bytes + consumed, remaining);
            *length = remaining;
        }
        *discarded_prefix = start;
        return true;
    }
    return false;
}

#endif /* GRAIN_SILO_RS485_STREAM_DECODER_H */
