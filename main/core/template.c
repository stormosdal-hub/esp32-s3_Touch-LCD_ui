/* Placeholder expansion for action fields, e.g. "PUMP {value:int}" or {"id":"{id}"}. */
#include "template.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "signals.h"
#include "util.h"

#define MAX_PAYLOAD 2048

typedef struct {
    char *buf;
    size_t len, cap;
} sbuf_t;

static void sb_put(sbuf_t *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 128;
        while (cap < sb->len + n + 1) {
            cap *= 2;
        }
        if (cap > MAX_PAYLOAD) {
            cap = MAX_PAYLOAD;
            if (sb->len + 1 >= cap) {
                return;
            }
            n = cap - sb->len - 1;
        }
        char *nb = realloc(sb->buf, cap);
        if (!nb) {
            return;
        }
        sb->buf = nb;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void put_value(sbuf_t *sb, double num, const char *text, const char *mod)
{
    char tmp[48];
    if (!mod || !*mod) {
        if (text) {
            sb_put(sb, text, strlen(text));
            return;
        }
        util_num_to_str(num, tmp, sizeof(tmp));
    } else if (!strcmp(mod, "hex")) {
        snprintf(tmp, sizeof(tmp), "%02X", (unsigned)((long)lround(num) & 0xFF));
    } else if (!strcmp(mod, "hex16")) {
        unsigned v = (unsigned)((long)lround(num) & 0xFFFF);
        snprintf(tmp, sizeof(tmp), "%02X %02X", v >> 8, v & 0xFF);
    } else if (!strcmp(mod, "hex16le")) {
        unsigned v = (unsigned)((long)lround(num) & 0xFFFF);
        snprintf(tmp, sizeof(tmp), "%02X %02X", v & 0xFF, v >> 8);
    } else if (!strcmp(mod, "int")) {
        snprintf(tmp, sizeof(tmp), "%ld", (long)lround(num));
    } else if (mod[0] == '.' && isdigit((unsigned char)mod[1])) {
        int prec = atoi(mod + 1);
        snprintf(tmp, sizeof(tmp), "%.*f", prec > 9 ? 9 : prec, num);
    } else {
        util_num_to_str(num, tmp, sizeof(tmp));
    }
    sb_put(sb, tmp, strlen(tmp));
}

/*
 * Placeholders: {value} {text} {id} {time} {date} {sig:NAME}, with optional
 * modifiers on value/sig: {value:hex} {value:hex16} {value:hex16le} {value:int} {value:.2}
 */
char *tmpl_expand(const char *tmpl, const tmpl_ctx_t *ctx)
{
    sbuf_t sb = {0};
    sb_put(&sb, "", 0);
    if (!tmpl) {
        return sb.buf;
    }
    const char *p = tmpl;
    while (*p) {
        const char *open = strchr(p, '{');
        if (!open) {
            sb_put(&sb, p, strlen(p));
            break;
        }
        sb_put(&sb, p, open - p);
        const char *close = strchr(open + 1, '}');
        const char *inner = strchr(open + 1, '{');
        if (!close || close - open > 64 || (inner && inner < close)) {
            /* not a placeholder (e.g. JSON braces): emit '{' and keep scanning */
            sb_put(&sb, open, 1);
            p = open + 1;
            continue;
        }
        char key[66];
        memcpy(key, open + 1, close - open - 1);
        key[close - open - 1] = '\0';
        char *mod = NULL;
        bool handled = true;

        if (!strncmp(key, "sig:", 4)) {
            char *name = key + 4;
            mod = strchr(name, ':');
            if (mod) {
                *mod++ = '\0';
            }
            int idx = sig_index(name, false);
            double v = 0;
            char txt[96];
            if (mod && *mod) {
                sig_get_num(idx, &v);
                put_value(&sb, v, NULL, mod);
            } else {
                sig_get_text(idx, txt, sizeof(txt));
                sb_put(&sb, txt, strlen(txt));
            }
        } else {
            mod = strchr(key, ':');
            if (mod) {
                *mod++ = '\0';
            }
            if (!strcmp(key, "value")) {
                put_value(&sb, ctx->num, (ctx->is_text && !mod) ? ctx->text : NULL, mod);
            } else if (!strcmp(key, "text")) {
                put_value(&sb, ctx->num, ctx->text, NULL);
            } else if (!strcmp(key, "id")) {
                sb_put(&sb, ctx->source ? ctx->source : "", ctx->source ? strlen(ctx->source) : 0);
            } else if (!strcmp(key, "time") || !strcmp(key, "date")) {
                time_t now = time(NULL);
                struct tm tm;
                localtime_r(&now, &tm);
                char tbuf[24];
                strftime(tbuf, sizeof(tbuf), key[0] == 't' ? "%H:%M:%S" : "%Y-%m-%d", &tm);
                sb_put(&sb, tbuf, strlen(tbuf));
            } else {
                handled = false;
            }
        }
        if (!handled) {
            sb_put(&sb, open, 1);
            p = open + 1;
            continue;
        }
        p = close + 1;
    }
    return sb.buf;
}
