#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Byte size of a format (u8 s8 u16 s16 u32 s32 f32, "le" suffix, bit0..bit7). 0 if unknown. */
int io_format_size(const char *fmt);
/* Decode bytes in `fmt` (big endian unless the format ends in "le"). */
bool io_decode(const uint8_t *b, int n, const char *fmt, double *out);
