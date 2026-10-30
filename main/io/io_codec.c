/* Binary value decoding shared by I2C, Modbus and CAN inputs. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "io_codec.h"

int io_format_size(const char *fmt)
{
    if (!strncmp(fmt, "bit", 3) || fmt[1] == '8') {
        return 1;
    }
    if (!strncmp(fmt + 1, "16", 2)) {
        return 2;
    }
    if (!strncmp(fmt + 1, "32", 2)) {
        return 4;
    }
    return 0;
}

bool io_decode(const uint8_t *b, int n, const char *fmt, double *out)
{
    int size = io_format_size(fmt);
    if (size == 0 || n < size) {
        return false;
    }
    if (!strncmp(fmt, "bit", 3)) {
        *out = (b[0] >> (atoi(fmt + 3) & 7)) & 1;
        return true;
    }
    bool le = strstr(fmt, "le") != NULL;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        v |= (uint32_t)b[le ? i : size - 1 - i] << (8 * i);
    }
    switch (fmt[0]) {
    case 'u':
        *out = v;
        break;
    case 's':
        *out = size == 1 ? (int8_t)v : size == 2 ? (int16_t)v : (int32_t)v;
        break;
    case 'f': {
        if (size != 4) {
            return false;
        }
        float f;
        memcpy(&f, &v, 4);
        *out = f;
        break;
    }
    default:
        return false;
    }
    return true;
}
