#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"

void *util_malloc_psram(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

const char *json_str(const cJSON *obj, const char *key, const char *def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) && it->valuestring ? it->valuestring : def;
}

double json_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        return it->valuedouble;
    }
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it) ? 1 : 0;
    }
    double v;
    if (cJSON_IsString(it) && util_parse_number(it->valuestring, &v, false)) {
        return v;
    }
    return def;
}

int json_int(const cJSON *obj, const char *key, int def)
{
    return (int)lround(json_num(obj, key, def));
}

bool json_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it);
    }
    if (cJSON_IsNumber(it)) {
        return it->valuedouble != 0;
    }
    return def;
}

bool util_parse_color(const char *s, uint32_t *rgb)
{
    if (!s) {
        return false;
    }
    if (*s == '#') {
        s++;
    }
    size_t n = strlen(s);
    if (n != 3 && n != 6) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    uint32_t v = strtoul(s, NULL, 16);
    if (n == 3) {
        v = ((v & 0xF00) << 12) | ((v & 0xF00) << 8) | ((v & 0x0F0) << 8) | ((v & 0x0F0) << 4) | ((v & 0x00F) << 4) | (v & 0x00F);
    }
    *rgb = v;
    return true;
}

void util_num_to_str(double v, char *out, size_t len)
{
    if (isfinite(v) && fabs(v) < 1e15 && v == floor(v)) {
        snprintf(out, len, "%.0f", v);
    } else {
        snprintf(out, len, "%.10g", v);
    }
}

void util_format_value(const char *fmt, double num, const char *text, char *out, size_t len)
{
    char numtxt[32];
    if (!text) {
        util_num_to_str(num, numtxt, sizeof(numtxt));
        text = numtxt;
    }
    if (len == 0) {
        return;
    }
    if (!fmt || !*fmt) {
        snprintf(out, len, "%s", text);
        return;
    }
    size_t o = 0;
    out[0] = '\0';
    const char *p = fmt;
    while (*p && o + 1 < len) {
        if (*p != '%') {
            out[o++] = *p++;
            continue;
        }
        if (p[1] == '%') {
            out[o++] = '%';
            p += 2;
            continue;
        }
        /* Parse %[flags][width][.precision]conv */
        const char *q = p + 1;
        while (*q && strchr("-+ 0#", *q)) {
            q++;
        }
        int digits = 0;
        while (isdigit((unsigned char)*q) && digits < 3) {
            q++;
            digits++;
        }
        if (*q == '.') {
            q++;
            digits = 0;
            while (isdigit((unsigned char)*q) && digits < 2) {
                q++;
                digits++;
            }
        }
        char conv = *q;
        if (!conv || !strchr("diuxXfFeEgGs", conv) || (size_t)(q - p) > 10) {
            out[o++] = *p++;
            continue;
        }
        char spec[16];
        size_t sl = q - p;
        memcpy(spec, p, sl);
        char buf[64];
        switch (conv) {
        case 'd': case 'i':
            spec[sl] = 'l'; spec[sl + 1] = 'd'; spec[sl + 2] = 0;
            snprintf(buf, sizeof(buf), spec, (long)llround(num));
            break;
        case 'u': case 'x': case 'X':
            spec[sl] = 'l'; spec[sl + 1] = conv; spec[sl + 2] = 0;
            snprintf(buf, sizeof(buf), spec, (unsigned long)(long)llround(num));
            break;
        case 's':
            spec[sl] = 's'; spec[sl + 1] = 0;
            snprintf(buf, sizeof(buf), spec, text);
            break;
        default:
            spec[sl] = conv; spec[sl + 1] = 0;
            snprintf(buf, sizeof(buf), spec, num);
            break;
        }
        size_t bl = strlen(buf);
        if (o + bl >= len) {
            bl = len - o - 1;
        }
        memcpy(out + o, buf, bl);
        o += bl;
        p = q + 1;
    }
    out[o] = '\0';
}

int util_parse_hex(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (s && *s) {
        while (*s && (isspace((unsigned char)*s) || *s == ',' || *s == ':' || *s == ';')) {
            s++;
        }
        if (!*s) {
            break;
        }
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s || v > 0xFF || n >= max) {
            return -1;
        }
        out[n++] = (uint8_t)v;
        s = end;
    }
    return n;
}

size_t util_unescape(char *s)
{
    char *w = s;
    const char *r = s;
    while (*r) {
        if (*r != '\\' || !r[1]) {
            *w++ = *r++;
            continue;
        }
        r++;
        switch (*r) {
        case 'r': *w++ = '\r'; r++; break;
        case 'n': *w++ = '\n'; r++; break;
        case 't': *w++ = '\t'; r++; break;
        case '0': *w++ = '\0'; r++; break;
        case '\\': *w++ = '\\'; r++; break;
        case 'x': {
            char hex[3] = {0};
            if (isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
                hex[0] = r[1];
                hex[1] = r[2];
                *w++ = (char)strtoul(hex, NULL, 16);
                r += 3;
            } else {
                *w++ = 'x';
                r++;
            }
            break;
        }
        default:
            *w++ = '\\';
            *w++ = *r++;
            break;
        }
    }
    *w = '\0';
    return w - s;
}

bool util_parse_number(const char *s, double *out, bool allow_words)
{
    if (!s) {
        return false;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (!*s) {
        return false;
    }
    char *end;
    double v = strtod(s, &end);
    if (end != s) {
        while (isspace((unsigned char)*end)) {
            end++;
        }
        if (!*end) {
            *out = v;
            return true;
        }
    }
    if (allow_words) {
        if (!strcasecmp(s, "true") || !strcasecmp(s, "on")) {
            *out = 1;
            return true;
        }
        if (!strcasecmp(s, "false") || !strcasecmp(s, "off")) {
            *out = 0;
            return true;
        }
    }
    return false;
}

cJSON *util_json_parse(const char *s, size_t len)
{
    if (!s) {
        return NULL;
    }
    int depth = 0;
    bool in_str = false;
    for (size_t i = 0; i < len && s[i]; i++) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > UTIL_JSON_MAX_DEPTH) {
                return NULL;
            }
        } else if (c == '}' || c == ']') {
            depth--;
        }
    }
    return cJSON_ParseWithLength(s, len);
}

uint32_t util_crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}
