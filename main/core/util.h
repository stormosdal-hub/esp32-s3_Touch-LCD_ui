#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* Allocate from PSRAM, falling back to internal RAM. */
void *util_malloc_psram(size_t size);

/* JSON helpers that tolerate missing keys and numbers given as strings. */
const char *json_str(const cJSON *obj, const char *key, const char *def);
double json_num(const cJSON *obj, const char *key, double def);
int json_int(const cJSON *obj, const char *key, int def);
bool json_bool(const cJSON *obj, const char *key, bool def);

/* "#RRGGBB" / "#RGB" / "RRGGBB" -> 0xRRGGBB. Returns false if not a colour. */
bool util_parse_color(const char *s, uint32_t *rgb);

/*
 * printf-style formatting of a single value without trusting the format string.
 * Each conversion (%d %i %u %x %X %f %F %e %E %g %G %s, with flags/width/precision)
 * is filled with the value; anything else is copied literally. "%%" prints "%".
 * An empty or NULL format prints the value as text.
 */
void util_format_value(const char *fmt, double num, const char *text, char *out, size_t len);

/* Format a number compactly (integers without decimals, otherwise up to 10 significant digits). */
void util_num_to_str(double v, char *out, size_t len);

/* Parse "01 02 ff", "0x01,0x02" or "01:02" into bytes. Returns count or -1 on error. */
int util_parse_hex(const char *s, uint8_t *out, int max);

/* Decode \r \n \t \\ \0 and \xHH escapes in place. Returns new length (data may contain NUL). */
size_t util_unescape(char *s);

/* Parse a whole string as a number. Accepts true/false/on/off as 1/0 when allow_words. */
bool util_parse_number(const char *s, double *out, bool allow_words);

uint32_t util_crc16_modbus(const uint8_t *data, size_t len);

/*
 * cJSON_ParseWithLength() for untrusted input: rejects documents nested deeper
 * than UTIL_JSON_MAX_DEPTH, since cJSON recurses per level and tasks have small stacks.
 */
#define UTIL_JSON_MAX_DEPTH 24
cJSON *util_json_parse(const char *s, size_t len);
