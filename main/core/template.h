#pragma once

#include <stdbool.h>

typedef struct {
    double num;           /* triggering value */
    const char *text;     /* optional text value (dropdown option, typed text, ...) */
    bool is_text;         /* {value} expands to text instead of num */
    const char *source;   /* widget or rule id, for {id} */
} tmpl_ctx_t;

/*
 * Expand {value} {text} {id} {time} {date} {sig:NAME} and modifiers
 * {value:hex} {value:hex16} {value:hex16le} {value:int} {value:.2} (also on sig).
 * Unknown {...} is copied unchanged. Returns a malloc'd string (free it).
 */
char *tmpl_expand(const char *tmpl, const tmpl_ctx_t *ctx);
