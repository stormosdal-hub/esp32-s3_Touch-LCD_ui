#include "ui_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "actions.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "io_manager.h"
#include "signals.h"
#include "util.h"
#include "web_server.h"

static const char *TAG = "ui";

#define MAX_WIDGETS   256
#define MAX_RULES     64
#define MAX_SERIES    4
#define LOG_LINE      120
#define TICK_MS       50
#define CHART_SCALE   100.0

typedef enum {
    W_LABEL, W_BUTTON, W_SWITCH, W_CHECKBOX, W_SLIDER, W_ARC, W_GAUGE, W_BAR,
    W_LED, W_DROPDOWN, W_CHART, W_LOG, W_PANEL, W_INPUT, W_UNKNOWN,
} wtype_t;

static const char *const s_type_names[] = {
    "label", "button", "switch", "checkbox", "slider", "arc", "gauge", "bar",
    "led", "dropdown", "chart", "log", "panel", "input",
};

typedef struct {
    char *buf;
    size_t len, cap;
    int lines, max_lines;
    bool newest_first;
    bool dirty;                  /* text changed since the label was last updated */
    const char *filter;          /* "*", list of signal names, or "" */
} logbuf_t;

typedef struct {
    wtype_t type;
    char id[32];
    lv_obj_t *obj;
    lv_obj_t *label;             /* inner label: button text, arc/gauge value, chart legend, log text */
    int sig;                     /* bound signal or -1 */
    char fmt[40];
    double mult;                 /* 10^decimals for slider/arc/bar */
    bool toggle;
    const cJSON *events;         /* points into the layout model */
    /* chart */
    lv_chart_series_t *ser[MAX_SERIES];
    int ser_sig[MAX_SERIES];
    int nser;
    lv_timer_t *timer;           /* chart sampling, or delayed release of a momentary button */
    uint32_t press_tick;
    /* log */
    logbuf_t *log;
} widget_t;

typedef enum { OP_CHANGE, OP_GT, OP_LT, OP_GE, OP_LE, OP_EQ, OP_NE } rule_op_t;

typedef struct {
    int sig;
    rule_op_t op;
    double num;
    const char *str;             /* compare as text when the rule value is a string */
    bool last;
    const cJSON *actions;
    char id[24];
} rule_t;

static struct {
    bool ws_signals;
    bool ws_events;
    int screensaver_s;
} s_set;

static lv_display_t *s_disp;
static cJSON *s_model;
static widget_t *s_w;
static int s_nw;
static rule_t *s_rules;
static int s_nr;
static lv_obj_t *s_tabview;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_wake;
static bool s_screen_off;
static int s_sig_tab = -1;
static int s_sig_backlight = -1;

/* ---------- Helpers ---------- */

static const lv_font_t *font_for(int size)
{
    static const struct {
        int size;
        const lv_font_t *font;
    } fonts[] = {
        {12, &lv_font_montserrat_12}, {14, &lv_font_montserrat_14}, {16, &lv_font_montserrat_16},
        {18, &lv_font_montserrat_18}, {20, &lv_font_montserrat_20}, {22, &lv_font_montserrat_22},
        {24, &lv_font_montserrat_24}, {28, &lv_font_montserrat_28}, {32, &lv_font_montserrat_32},
        {36, &lv_font_montserrat_36}, {40, &lv_font_montserrat_40}, {48, &lv_font_montserrat_48},
    };
    const lv_font_t *best = fonts[0].font;
    int best_d = 1000;
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        int d = abs(fonts[i].size - size);
        if (d < best_d) {
            best_d = d;
            best = fonts[i].font;
        }
    }
    return best;
}

static bool color_of(const cJSON *j, const char *key, lv_color_t *out)
{
    uint32_t rgb;
    if (!util_parse_color(json_str(j, key, NULL), &rgb)) {
        return false;
    }
    *out = lv_color_hex(rgb);
    return true;
}

static wtype_t type_of(const char *name)
{
    for (int i = 0; i < W_UNKNOWN; i++) {
        if (!strcmp(name, s_type_names[i])) {
            return (wtype_t)i;
        }
    }
    return W_UNKNOWN;
}

static void set_checked(lv_obj_t *obj, bool on)
{
    if (on) {
        lv_obj_add_state(obj, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_CHECKED);
    }
}

static int32_t to_lv(const widget_t *w, double v)
{
    double s = v * w->mult;
    if (s > INT32_MAX / 2) {
        s = INT32_MAX / 2;
    } else if (s < INT32_MIN / 2) {
        s = INT32_MIN / 2;
    }
    return (int32_t)lround(s);
}

static bool log_accepts(const logbuf_t *lb, const char *name)
{
    if (!lb->filter || !*lb->filter) {
        return false;
    }
    if (!strcmp(lb->filter, "*")) {
        return strncmp(name, "sys.", 4) != 0;
    }
    size_t n = strlen(name);
    const char *p = lb->filter;
    while (*p) {
        while (*p == ',' || *p == ' ') {
            p++;
        }
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') {
            e++;
        }
        if ((size_t)(e - p) == n && !strncmp(p, name, n)) {
            return true;
        }
        p = e;
    }
    return false;
}

static void log_append(widget_t *w, const char *text)
{
    logbuf_t *lb = w->log;
    char entry[LOG_LINE + 16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int o = strftime(entry, sizeof(entry), "%H:%M:%S ", &tm);
    snprintf(entry + o, sizeof(entry) - o, "%s", text);
    for (char *c = entry; *c; c++) {
        if (*c == '\n' || *c == '\r') {
            *c = ' ';
        }
    }
    size_t el = strlen(entry) + 1;   /* with '\n' */

    /* Drop old lines until the new one fits. */
    while (lb->len && (lb->lines >= lb->max_lines || lb->len + el + 1 > lb->cap)) {
        if (lb->newest_first) {
            /* drop the last line: find the newline before the trailing one */
            size_t k = lb->len >= 2 ? lb->len - 2 : 0;
            while (k > 0 && lb->buf[k] != '\n') {
                k--;
            }
            lb->len = (lb->buf[k] == '\n') ? k + 1 : 0;
        } else {
            char *nl = memchr(lb->buf, '\n', lb->len);
            size_t drop = nl ? (size_t)(nl - lb->buf + 1) : lb->len;
            memmove(lb->buf, lb->buf + drop, lb->len - drop);
            lb->len -= drop;
        }
        lb->lines--;
    }
    if (el + 1 > lb->cap) {
        return;
    }
    if (lb->newest_first) {
        memmove(lb->buf + el, lb->buf, lb->len);
        memcpy(lb->buf, entry, el - 1);
        lb->buf[el - 1] = '\n';
    } else {
        memcpy(lb->buf + lb->len, entry, el - 1);
        lb->buf[lb->len + el - 1] = '\n';
    }
    lb->len += el;
    lb->lines++;
    lb->buf[lb->len] = '\0';
    lb->dirty = true;
}

/* Push pending log text to the label (once per tick, however many lines arrived). */
static void log_flush(widget_t *w)
{
    logbuf_t *lb = w->log;
    if (!lb->dirty) {
        return;
    }
    lb->dirty = false;
    if (lb->len) {
        lb->buf[lb->len - 1] = '\0';     /* hide trailing newline */
    }
    lv_label_set_text(w->label, lb->buf);
    if (lb->len) {
        lb->buf[lb->len - 1] = '\n';
    }
    if (!lb->newest_first) {
        lv_obj_update_layout(w->obj);
        lv_obj_scroll_to_y(w->obj, lv_obj_get_scroll_y(w->obj) + lv_obj_get_scroll_bottom(w->obj), LV_ANIM_OFF);
    }
}

/* only_sig: -1 = push all series, -2 = refresh legend only, else push series bound to that signal */
static void chart_push(widget_t *w, int only_sig)
{
    char legend[160] = "";
    int lo = 0;
    for (int i = 0; i < w->nser; i++) {
        double v;
        bool has = sig_get_num(w->ser_sig[i], &v);
        if (only_sig == -1 || w->ser_sig[i] == only_sig) {
            int32_t p = has ? (int32_t)lround(fmin(fmax(v * CHART_SCALE, -2e9), 2e9)) : LV_CHART_POINT_NONE;
            lv_chart_set_next_value(w->obj, w->ser[i], p);
        }
        if (w->label && has && lo < (int)sizeof(legend) - 1) {
            char val[48];
            util_format_value(w->fmt, v, NULL, val, sizeof(val));
            lo += snprintf(legend + lo, sizeof(legend) - lo, "%s%s: %s", i ? "\n" : "", sig_name(w->ser_sig[i]), val);
        }
    }
    if (w->label) {
        lv_label_set_text(w->label, legend);
    }
}

static void chart_timer_cb(lv_timer_t *t)
{
    chart_push(lv_timer_get_user_data(t), -1);
}

/* Push the bound signal's current value into a widget (skipped while the user drags it unless forced). */
static void widget_update_ex(widget_t *w, bool force)
{
    if (w->sig < 0 || w->type == W_CHART || w->type == W_LOG) {
        return;
    }
    double v;
    if (!sig_get_num(w->sig, &v)) {
        return;
    }
    char txt[128];
    sig_get_text(w->sig, txt, sizeof(txt));
    bool is_text = sig_is_text(w->sig);
    bool busy = !force && lv_obj_has_state(w->obj, LV_STATE_PRESSED);
    char out[192];

    switch (w->type) {
    case W_LABEL:
        util_format_value(w->fmt, v, is_text ? txt : NULL, out, sizeof(out));
        lv_label_set_text(w->obj, out);
        break;
    case W_BUTTON:
        if (w->toggle) {
            set_checked(w->obj, v != 0);
        }
        break;
    case W_SWITCH:
    case W_CHECKBOX:
        if (!busy) {
            set_checked(w->obj, v != 0);
        }
        break;
    case W_SLIDER:
        if (!busy) {
            lv_slider_set_value(w->obj, to_lv(w, v), LV_ANIM_OFF);
        }
        break;
    case W_ARC:
    case W_GAUGE:
        if (!busy) {
            lv_arc_set_value(w->obj, to_lv(w, v));
        }
        if (w->label) {
            util_format_value(w->fmt, v, is_text ? txt : NULL, out, sizeof(out));
            lv_label_set_text(w->label, out);
        }
        break;
    case W_BAR:
        lv_bar_set_value(w->obj, to_lv(w, v), LV_ANIM_OFF);
        if (w->label) {
            util_format_value(w->fmt, v, NULL, out, sizeof(out));
            lv_label_set_text(w->label, out);
        }
        break;
    case W_LED:
        if (v != 0) {
            lv_led_on(w->obj);
        } else {
            lv_led_off(w->obj);
        }
        break;
    case W_DROPDOWN: {
        int idx = (int)v;
        if (is_text) {
            /* select the option whose text equals the signal */
            idx = -1;
            const char *p = lv_dropdown_get_options(w->obj);
            size_t tl = strlen(txt);
            for (int i = 0; p && *p; i++) {
                const char *e = strchr(p, '\n');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                if (len == tl && !strncmp(p, txt, tl)) {
                    idx = i;
                    break;
                }
                p = e ? e + 1 : NULL;
            }
        }
        if (idx >= 0 && idx < (int)lv_dropdown_get_option_count(w->obj)) {
            lv_dropdown_set_selected(w->obj, idx);
        }
        break;
    }
    case W_INPUT:
        if (!lv_obj_has_state(w->obj, LV_STATE_FOCUSED)) {
            lv_textarea_set_text(w->obj, txt);
        }
        break;
    default:
        break;
    }
}

static void widget_update(widget_t *w)
{
    widget_update_ex(w, false);
}

/* ---------- Screen saver ---------- */

static void wake_cb(lv_event_t *e)
{
    if (s_wake) {
        lv_obj_delete_async(s_wake);
        s_wake = NULL;
    }
    s_screen_off = false;
    board_backlight_set(true);
    lv_display_trigger_activity(s_disp);
    if (s_sig_backlight >= 0) {
        sig_set_num_idx(s_sig_backlight, 1, false);
    }
}

static void screen_off(void)
{
    if (s_screen_off) {
        return;
    }
    s_screen_off = true;
    board_backlight_set(false);
    /* Invisible layer that swallows the first touch so it only wakes the screen. */
    s_wake = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake);
    lv_obj_set_size(s_wake, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_wake, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wake, wake_cb, LV_EVENT_PRESSED, NULL);
    if (s_sig_backlight >= 0) {
        sig_set_num_idx(s_sig_backlight, 0, false);
    }
}

/* ---------- Widget events ---------- */

static double widget_value(widget_t *w, char *text, size_t tlen, bool *is_text, lv_event_code_t code)
{
    *is_text = false;
    text[0] = '\0';
    switch (w->type) {
    case W_BUTTON:
        if (w->toggle) {
            return lv_obj_has_state(w->obj, LV_STATE_CHECKED);
        }
        return code == LV_EVENT_RELEASED ? 0 : 1;
    case W_SWITCH:
    case W_CHECKBOX:
        return lv_obj_has_state(w->obj, LV_STATE_CHECKED);
    case W_SLIDER:
        return lv_slider_get_value(w->obj) / w->mult;
    case W_ARC:
        return lv_arc_get_value(w->obj) / w->mult;
    case W_DROPDOWN:
        lv_dropdown_get_selected_str(w->obj, text, tlen);
        return lv_dropdown_get_selected(w->obj);
    case W_INPUT:
        strlcpy(text, lv_textarea_get_text(w->obj), tlen);
        *is_text = true;
        return 0;
    default:
        return 1;
    }
}

static void keyboard_hide(void)
{
    if (s_keyboard) {
        lv_keyboard_set_textarea(s_keyboard, NULL);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void keyboard_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    /* READY (the OK key) is forwarded by LVGL to the textarea next, where
     * event_cb submits and hides; hiding here would detach the textarea first. */
    if (code == LV_EVENT_CANCEL) {
        lv_obj_t *ta = lv_keyboard_get_textarea(s_keyboard);
        if (ta) {
            lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        }
        keyboard_hide();
    }
}

static void keyboard_show(widget_t *w)
{
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(s_keyboard, LV_PCT(100), LV_PCT(45));
        lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_ALL, NULL);
    }
    bool numeric = !strcmp(w->fmt, "number");
    lv_keyboard_set_mode(s_keyboard, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_keyboard, w->obj);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void release_timer_cb(lv_timer_t *t)
{
    widget_t *w = lv_timer_get_user_data(t);
    sig_set_num_idx(w->sig, 0, false);
}

/*
 * Write 0 for a released momentary button. A tap shorter than two UI ticks
 * would otherwise be 1 and 0 within one tick, and rules/WebSocket/MQTT would
 * never see the 1, so the 0 is delayed in that case.
 */
static void release_momentary(widget_t *w)
{
    if (lv_tick_elaps(w->press_tick) >= 2 * TICK_MS) {
        sig_set_num_idx(w->sig, 0, false);
        return;
    }
    if (!w->timer) {
        w->timer = lv_timer_create(release_timer_cb, 2 * TICK_MS, w);
        lv_timer_set_auto_delete(w->timer, false);
    }
    lv_timer_set_repeat_count(w->timer, 1);
    lv_timer_reset(w->timer);
    lv_timer_resume(w->timer);
}

static void event_cb(lv_event_t *e)
{
    widget_t *w = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    const char *name;
    switch (code) {
    case LV_EVENT_CLICKED:      name = "clicked"; break;
    case LV_EVENT_PRESSED:      name = "pressed"; break;
    case LV_EVENT_RELEASED:     name = "released"; break;
    case LV_EVENT_LONG_PRESSED: name = "long_pressed"; break;
    case LV_EVENT_VALUE_CHANGED: name = "changed"; break;
    case LV_EVENT_READY:        name = "submit"; break;
    case LV_EVENT_FOCUSED:
        if (w->type == W_INPUT) {
            keyboard_show(w);
        }
        return;
    case LV_EVENT_DEFOCUSED:
        if (w->type == W_INPUT && s_keyboard && lv_keyboard_get_textarea(s_keyboard) == w->obj) {
            keyboard_hide();
        }
        return;
    default:
        return;
    }
    if (w->type == W_INPUT && code == LV_EVENT_VALUE_CHANGED) {
        return;     /* every keystroke; only "submit" is interesting */
    }
    if (w->type == W_INPUT && code == LV_EVENT_CLICKED) {
        keyboard_show(w);   /* tapping an already focused input sends no FOCUSED */
    }
    if (w->type == W_INPUT && code == LV_EVENT_READY) {
        keyboard_hide();
        lv_obj_remove_state(w->obj, LV_STATE_FOCUSED);
    }

    char text[96];
    bool is_text;
    double v = widget_value(w, text, sizeof(text), &is_text, code);

    /* Two-way binding: user changes write back to the bound signal. */
    if (w->sig >= 0) {
        const char *sname = sig_name(w->sig);
        bool momentary = w->type == W_BUTTON && !w->toggle;
        if (momentary && code == LV_EVENT_PRESSED) {
            w->press_tick = lv_tick_get();
            sig_set_num(sname, 1, false);
        } else if (momentary && code == LV_EVENT_RELEASED) {
            release_momentary(w);
        } else if (w->type == W_DROPDOWN && code == LV_EVENT_VALUE_CHANGED && sig_is_text(w->sig)) {
            sig_set_text(sname, text, false);   /* text signal: keep it text */
        } else if (!momentary && code == LV_EVENT_VALUE_CHANGED) {
            sig_set_num(sname, v, false);
        } else if (w->type == W_INPUT && code == LV_EVENT_READY) {
            sig_set_text(sname, text, true);
        }
        if (!momentary && code == LV_EVENT_RELEASED) {
            /* updates that arrived while the user held the widget were skipped */
            widget_update_ex(w, true);
        }
    }

    if (s_set.ws_events && (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED)) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "event", name);
        cJSON_AddStringToObject(m, "widget", w->id);
        if (is_text) {
            cJSON_AddStringToObject(m, "value", text);
        } else {
            cJSON_AddNumberToObject(m, "value", v);
            if (text[0]) {
                cJSON_AddStringToObject(m, "text", text);
            }
        }
        char *s = cJSON_PrintUnformatted(m);
        if (s) {
            web_ws_broadcast(s);
            cJSON_free(s);
        }
        cJSON_Delete(m);
    }

    const cJSON *ev;
    cJSON_ArrayForEach(ev, w->events) {
        if (!strcmp(json_str(ev, "on", ""), name)) {
            actions_run(cJSON_GetObjectItem(ev, "actions"), v, text[0] ? text : NULL, is_text, w->id);
        }
    }
}

/* ---------- Building ---------- */

static void style_common(lv_obj_t *obj, const cJSON *j)
{
    lv_color_t c;
    if (color_of(j, "bg", &c)) {
        lv_obj_set_style_bg_color(obj, c, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    if (color_of(j, "fg", &c)) {
        lv_obj_set_style_text_color(obj, c, 0);
    }
    if (color_of(j, "border_color", &c)) {
        lv_obj_set_style_border_color(obj, c, 0);
    }
    int bw = json_int(j, "border_width", -1);
    if (bw >= 0) {
        lv_obj_set_style_border_width(obj, bw, 0);
    }
    int r = json_int(j, "radius", -1);
    if (r >= 0) {
        lv_obj_set_style_radius(obj, r, 0);
    }
    int fs = json_int(j, "font", 0);
    if (fs > 0) {
        lv_obj_set_style_text_font(obj, font_for(fs), 0);
    }
    const char *align = json_str(j, "align", "");
    if (*align) {
        lv_obj_set_style_text_align(obj, !strcmp(align, "center") ? LV_TEXT_ALIGN_CENTER
                                    : !strcmp(align, "right") ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    }
}

static void style_accent(lv_obj_t *obj, const cJSON *j, bool arc)
{
    lv_color_t c;
    if (!color_of(j, "accent", &c)) {
        return;
    }
    if (arc) {
        lv_obj_set_style_arc_color(obj, c, LV_PART_INDICATOR);
    } else {
        lv_obj_set_style_bg_color(obj, c, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(obj, c, LV_PART_INDICATOR | LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(obj, c, LV_PART_KNOB);
}

static lv_obj_t *center_label(lv_obj_t *parent, const cJSON *j)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_center(l);
    lv_color_t c;
    if (color_of(j, "fg", &c)) {
        lv_obj_set_style_text_color(l, c, 0);
    }
    int fs = json_int(j, "font", 0);
    if (fs > 0) {
        lv_obj_set_style_text_font(l, font_for(fs), 0);
    }
    return l;
}

static void build_range(widget_t *w, const cJSON *j, double def_max)
{
    int dec = json_int(j, "decimals", 0);
    dec = dec < 0 ? 0 : dec > 3 ? 3 : dec;
    w->mult = pow(10, dec);
    if (!w->fmt[0]) {
        snprintf(w->fmt, sizeof(w->fmt), "%%.%df", dec);
    }
    int32_t lo = to_lv(w, json_num(j, "min", 0));
    int32_t hi = to_lv(w, json_num(j, "max", def_max));
    if (hi <= lo) {
        hi = lo + 1;
    }
    switch (w->type) {
    case W_SLIDER: lv_slider_set_range(w->obj, lo, hi); break;
    case W_BAR:    lv_bar_set_range(w->obj, lo, hi); break;
    default:       lv_arc_set_range(w->obj, lo, hi); break;
    }
}

static void build_chart(widget_t *w, const cJSON *j)
{
    lv_obj_t *ch = w->obj;
    lv_chart_set_type(ch, !strcmp(json_str(j, "chart_type", "line"), "bar") ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
    int points = json_int(j, "points", 60);
    lv_chart_set_point_count(ch, points < 2 ? 2 : points > 500 ? 500 : points);
    lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, (int32_t)(json_num(j, "min", 0) * CHART_SCALE),
                            (int32_t)(json_num(j, "max", 100) * CHART_SCALE));
    lv_chart_set_div_line_count(ch, 5, 8);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_size(ch, 0, 0, LV_PART_INDICATOR);   /* no point markers */

    static const uint32_t palette[MAX_SERIES] = { 0x2196F3, 0xFF9800, 0x4CAF50, 0xE91E63 };
    const cJSON *series = cJSON_GetObjectItem(j, "series");
    const cJSON *s;
    cJSON_ArrayForEach(s, series) {
        if (w->nser >= MAX_SERIES) {
            break;
        }
        int idx = sig_index(json_str(s, "signal", ""), true);
        if (idx < 0) {
            continue;
        }
        lv_color_t c = lv_color_hex(palette[w->nser]);
        color_of(s, "color", &c);
        w->ser[w->nser] = lv_chart_add_series(ch, c, LV_CHART_AXIS_PRIMARY_Y);
        w->ser_sig[w->nser++] = idx;
    }
    if (json_bool(j, "legend", true) && w->nser) {
        w->label = lv_label_create(ch);
        lv_obj_align(w->label, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_font(w->label, &lv_font_montserrat_12, 0);
        if (!w->fmt[0]) {
            strlcpy(w->fmt, "%.1f", sizeof(w->fmt));
        }
    }
    int sample = json_int(j, "sample_ms", 1000);
    if (sample > 0 && w->nser) {
        w->timer = lv_timer_create(chart_timer_cb, sample < 50 ? 50 : sample, w);
    }
}

static void build_log(widget_t *w, const cJSON *j)
{
    lv_obj_set_scroll_dir(w->obj, LV_DIR_VER);
    w->label = lv_label_create(w->obj);
    lv_obj_set_width(w->label, LV_PCT(100));
    lv_label_set_long_mode(w->label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(w->label, "");
    int max = json_int(j, "max_lines", 50);
    max = max < 1 ? 1 : max > 500 ? 500 : max;
    w->log = calloc(1, sizeof(logbuf_t));
    if (!w->log) {
        return;
    }
    w->log->max_lines = max;
    w->log->cap = (size_t)max * (LOG_LINE + 16) + 2;
    w->log->buf = heap_caps_malloc(w->log->cap, MALLOC_CAP_SPIRAM);
    w->log->newest_first = json_bool(j, "newest_first", false);
    w->log->filter = json_str(j, "signals", "*");
    if (!w->log->buf) {
        free(w->log);
        w->log = NULL;
    } else {
        w->log->buf[0] = '\0';
    }
}

static void build_widget(lv_obj_t *page, const cJSON *j)
{
    if (s_nw >= MAX_WIDGETS) {
        return;
    }
    wtype_t type = type_of(json_str(j, "type", ""));
    if (type == W_UNKNOWN) {
        ESP_LOGW(TAG, "unknown widget type '%s'", json_str(j, "type", ""));
        return;
    }
    widget_t *w = &s_w[s_nw];
    memset(w, 0, sizeof(*w));
    w->type = type;
    w->sig = -1;
    w->mult = 1;
    strlcpy(w->id, json_str(j, "id", ""), sizeof(w->id));
    strlcpy(w->fmt, json_str(j, "format", ""), sizeof(w->fmt));
    w->events = cJSON_GetObjectItem(j, "events");
    const char *text = json_str(j, "text", "");
    const char *signal = json_str(j, "signal", "");
    if (*signal && type != W_CHART && type != W_LOG) {
        w->sig = sig_index(signal, true);
    }

    lv_obj_t *o = NULL;
    switch (type) {
    case W_LABEL:
        o = lv_label_create(page);
        lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
        lv_label_set_text(o, text);
        break;
    case W_BUTTON:
        o = lv_button_create(page);
        w->toggle = json_bool(j, "toggle", false);
        if (w->toggle) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_CHECKABLE);
        }
        w->label = center_label(o, j);
        lv_label_set_text(w->label, text);
        break;
    case W_SWITCH:
        o = lv_switch_create(page);
        style_accent(o, j, false);
        break;
    case W_CHECKBOX:
        o = lv_checkbox_create(page);
        lv_checkbox_set_text(o, text);
        style_accent(o, j, false);
        break;
    case W_SLIDER:
        o = lv_slider_create(page);
        w->obj = o;
        build_range(w, j, 100);
        style_accent(o, j, false);
        break;
    case W_ARC:
    case W_GAUGE:
        o = lv_arc_create(page);
        w->obj = o;
        build_range(w, j, 100);
        lv_arc_set_bg_angles(o, 135, 45);
        style_accent(o, j, true);
        {
            int thick = json_int(j, "thickness", 0);
            if (thick > 0) {
                lv_obj_set_style_arc_width(o, thick, LV_PART_MAIN);
                lv_obj_set_style_arc_width(o, thick, LV_PART_INDICATOR);
            }
            lv_color_t c;
            if (color_of(j, "track", &c)) {
                lv_obj_set_style_arc_color(o, c, LV_PART_MAIN);
            }
        }
        if (type == W_GAUGE) {
            lv_obj_remove_style(o, NULL, LV_PART_KNOB);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        }
        if (type == W_GAUGE || json_bool(j, "show_value", true)) {
            w->label = center_label(o, j);
            lv_label_set_text(w->label, "");
        }
        break;
    case W_BAR:
        o = lv_bar_create(page);
        w->obj = o;
        build_range(w, j, 100);
        style_accent(o, j, false);
        if (json_bool(j, "show_value", false)) {
            w->label = center_label(o, j);
            lv_label_set_text(w->label, "");
        }
        break;
    case W_LED: {
        o = lv_led_create(page);
        lv_color_t c = lv_palette_main(LV_PALETTE_GREEN);
        color_of(j, "accent", &c);
        lv_led_set_color(o, c);
        lv_led_off(o);
        break;
    }
    case W_DROPDOWN:
        o = lv_dropdown_create(page);
        lv_dropdown_set_options(o, json_str(j, "options", "Option 1\nOption 2"));
        break;
    case W_CHART:
        o = lv_chart_create(page);
        w->obj = o;
        build_chart(w, j);
        break;
    case W_LOG:
        o = lv_obj_create(page);
        w->obj = o;
        build_log(w, j);
        break;
    case W_PANEL:
        o = lv_obj_create(page);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        if (*text) {
            lv_obj_t *l = lv_label_create(o);
            lv_label_set_text(l, text);
            lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
        }
        break;
    case W_INPUT:
        o = lv_textarea_create(page);
        lv_textarea_set_one_line(o, true);
        lv_textarea_set_placeholder_text(o, text);
        if (json_bool(j, "numeric", false)) {
            strlcpy(w->fmt, "number", sizeof(w->fmt));
            lv_textarea_set_accepted_chars(o, "0123456789.-");
        }
        break;
    default:
        return;
    }
    w->obj = o;

    lv_obj_set_pos(o, json_int(j, "x", 0), json_int(j, "y", 0));
    lv_obj_set_size(o, json_int(j, "w", 100), json_int(j, "h", 40));
    style_common(o, j);
    if (type == W_LABEL && json_bool(j, "center_v", false)) {
        lv_obj_set_style_pad_top(o, (json_int(j, "h", 40) - lv_font_get_line_height(font_for(json_int(j, "font", 16)))) / 2, 0);
    }

    /* Events: interactive widgets always (for WS events and binding), others only if events exist. */
    bool interactive = type == W_BUTTON || type == W_SWITCH || type == W_CHECKBOX || type == W_SLIDER ||
                       type == W_ARC || type == W_DROPDOWN || type == W_INPUT;
    if (!interactive && cJSON_GetArraySize(w->events) > 0) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        interactive = true;
    }
    if (interactive) {
        static const lv_event_code_t codes[] = {
            LV_EVENT_CLICKED, LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_LONG_PRESSED,
            LV_EVENT_VALUE_CHANGED, LV_EVENT_READY, LV_EVENT_FOCUSED, LV_EVENT_DEFOCUSED,
        };
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
            lv_obj_add_event_cb(o, event_cb, codes[i], w);
        }
    }
    if (json_bool(j, "hidden", false)) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    s_nw++;
}

static void build_rules(const cJSON *rules)
{
    int n = cJSON_GetArraySize(rules);
    if (n > MAX_RULES) {
        n = MAX_RULES;
    }
    s_rules = n ? calloc(n, sizeof(rule_t)) : NULL;
    s_nr = 0;
    for (int i = 0; s_rules && i < n; i++) {
        const cJSON *j = cJSON_GetArrayItem(rules, i);
        rule_t *r = &s_rules[s_nr];
        r->sig = sig_index(json_str(j, "signal", ""), true);
        if (r->sig < 0) {
            continue;
        }
        const char *op = json_str(j, "op", "change");
        static const char *const ops[] = { "change", ">", "<", ">=", "<=", "==", "!=" };
        r->op = OP_CHANGE;
        for (int k = 0; k < 7; k++) {
            if (!strcmp(op, ops[k])) {
                r->op = (rule_op_t)k;
            }
        }
        const cJSON *val = cJSON_GetObjectItem(j, "value");
        if (cJSON_IsString(val) && !util_parse_number(val->valuestring, &r->num, false)) {
            r->str = val->valuestring;
        } else {
            r->num = json_num(j, "value", 0);
        }
        r->actions = cJSON_GetObjectItem(j, "actions");
        strlcpy(r->id, json_str(j, "id", "rule"), sizeof(r->id));
        s_nr++;
    }
}

static void rules_eval(int idx)
{
    for (int i = 0; i < s_nr; i++) {
        rule_t *r = &s_rules[i];
        if (r->sig != idx) {
            continue;
        }
        double v = 0;
        char txt[96];
        sig_get_num(idx, &v);
        sig_get_text(idx, txt, sizeof(txt));
        bool cond;
        switch (r->op) {
        case OP_GT: cond = v > r->num; break;
        case OP_LT: cond = v < r->num; break;
        case OP_GE: cond = v >= r->num; break;
        case OP_LE: cond = v <= r->num; break;
        case OP_EQ: cond = r->str ? !strcmp(txt, r->str) : v == r->num; break;
        case OP_NE: cond = r->str ? strcmp(txt, r->str) != 0 : v != r->num; break;
        default:    cond = true; break;
        }
        if (cond && (r->op == OP_CHANGE || !r->last)) {
            actions_run(r->actions, v, txt, sig_is_text(idx), r->id);
        }
        r->last = cond;
    }
}

static void teardown(void)
{
    for (int i = 0; i < s_nw; i++) {
        if (s_w[i].timer) {
            lv_timer_delete(s_w[i].timer);
        }
        if (s_w[i].log) {
            heap_caps_free(s_w[i].log->buf);
            free(s_w[i].log);
        }
    }
    s_nw = 0;
    free(s_rules);
    s_rules = NULL;
    s_nr = 0;
    if (s_keyboard) {
        lv_obj_delete(s_keyboard);
        s_keyboard = NULL;
    }
    lv_obj_clean(lv_screen_active());
    s_tabview = NULL;
}

static void tabview_cb(lv_event_t *e)
{
    if (s_sig_tab >= 0) {
        sig_set_num_idx(s_sig_tab, lv_tabview_get_tab_active(s_tabview), false);
    }
}

void ui_engine_build(cJSON *layout)
{
    teardown();
    cJSON_Delete(s_model);
    s_model = layout;

    const cJSON *set = cJSON_GetObjectItem(layout, "settings");
    s_set.ws_signals = json_bool(set, "ws_signals", true);
    s_set.ws_events = json_bool(set, "ws_events", true);
    s_set.screensaver_s = json_int(set, "screensaver_s", 0);

    /* Initial signal values (only for signals without a value yet). */
    const cJSON *sigs = cJSON_GetObjectItem(layout, "signals");
    const cJSON *sj;
    cJSON_ArrayForEach(sj, sigs) {
        const char *name = json_str(sj, "name", "");
        int idx = sig_index(name, true);
        double tmp;
        const cJSON *val = cJSON_GetObjectItem(sj, "value");
        if (idx >= 0 && !sig_get_num(idx, &tmp) && val) {
            if (cJSON_IsString(val)) {
                sig_set_text(name, val->valuestring, false);
            } else {
                sig_set_num(name, json_num(sj, "value", 0), false);
            }
        }
    }

    /* Theme */
    uint32_t primary = 0x2196F3, secondary = 0xFF9800;
    util_parse_color(json_str(set, "primary", ""), &primary);
    util_parse_color(json_str(set, "secondary", ""), &secondary);
    bool dark = strcmp(json_str(set, "theme", "dark"), "light") != 0;
    lv_theme_t *th = lv_theme_default_init(s_disp, lv_color_hex(primary), lv_color_hex(secondary), dark,
                                           font_for(json_int(set, "font", 16)));
    lv_display_set_theme(s_disp, th);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_color_t bg = dark ? lv_color_hex(0x15171A) : lv_color_hex(0xF2F3F5);
    color_of(set, "bg", &bg);
    lv_obj_set_style_bg_color(scr, bg, 0);

    /* Tabs */
    s_tabview = lv_tabview_create(scr);
    const char *pos = json_str(set, "tabbar", "top");
    lv_dir_t dir = !strcmp(pos, "bottom") ? LV_DIR_BOTTOM : !strcmp(pos, "left") ? LV_DIR_LEFT
                   : !strcmp(pos, "right") ? LV_DIR_RIGHT : LV_DIR_TOP;
    lv_tabview_set_tab_bar_position(s_tabview, dir);
    lv_tabview_set_tab_bar_size(s_tabview, json_int(set, "tabbar_size", 50));
    lv_obj_set_style_bg_opa(s_tabview, LV_OPA_TRANSP, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabview);
    if (!strcmp(pos, "none")) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }
    int tab_font = json_int(set, "tab_font", 0);
    if (tab_font > 0) {
        lv_obj_set_style_text_font(bar, font_for(tab_font), 0);
    }
    if (!json_bool(set, "swipe", false)) {
        lv_obj_remove_flag(lv_tabview_get_content(s_tabview), LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_event_cb(s_tabview, tabview_cb, LV_EVENT_VALUE_CHANGED, NULL);

    const cJSON *tabs = cJSON_GetObjectItem(layout, "tabs");
    const cJSON *tab;
    cJSON_ArrayForEach(tab, tabs) {
        lv_obj_t *page = lv_tabview_add_tab(s_tabview, json_str(tab, "title", "Tab"));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_color_t c;
        if (color_of(tab, "bg", &c)) {
            lv_obj_set_style_bg_color(page, c, 0);
            lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
        }
        const cJSON *wj;
        cJSON_ArrayForEach(wj, cJSON_GetObjectItem(tab, "widgets")) {
            build_widget(page, wj);
        }
    }
    if (lv_tabview_get_tab_count(s_tabview) == 0) {
        lv_obj_t *page = lv_tabview_add_tab(s_tabview, "Empty");
        lv_obj_t *l = lv_label_create(page);
        lv_label_set_text(l, "This layout has no tabs.\nOpen the web editor to add some.");
        lv_obj_center(l);
    }

    build_rules(cJSON_GetObjectItem(layout, "rules"));

    for (int i = 0; i < s_nw; i++) {
        widget_update(&s_w[i]);
        if (s_w[i].type == W_CHART) {
            chart_push(&s_w[i], -2);  /* legend only */
        }
    }
    if (s_sig_tab >= 0) {
        sig_set_num_idx(s_sig_tab, 0, false);
    }
    ESP_LOGI(TAG, "UI built: %d widgets, %d rules, %d tabs", s_nw, s_nr, (int)lv_tabview_get_tab_count(s_tabview));
}

void ui_engine_show_tab(const char *tab)
{
    if (!s_tabview || !tab || !*tab) {
        return;
    }
    int i = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(s_model, "tabs")) {
        if (!strcmp(json_str(t, "id", ""), tab) || !strcmp(json_str(t, "title", ""), tab)) {
            lv_tabview_set_active(s_tabview, i, LV_ANIM_OFF);
            tabview_cb(NULL);
            return;
        }
        i++;
    }
    double v;
    if (util_parse_number(tab, &v, false) && v >= 0 && v < lv_tabview_get_tab_count(s_tabview)) {
        lv_tabview_set_active(s_tabview, (uint32_t)v, LV_ANIM_OFF);
        tabview_cb(NULL);
    }
}

void ui_engine_log(const char *text)
{
    for (int i = 0; i < s_nw; i++) {
        if (s_w[i].log) {
            log_append(&s_w[i], text);
        }
    }
}

/* ---------- Periodic processing ---------- */

typedef struct {
    cJSON *batch;
} tick_ctx_t;

static void on_dirty(int idx, void *arg)
{
    tick_ctx_t *ctx = arg;
    char txt[128];
    sig_get_text(idx, txt, sizeof(txt));
    const char *name = sig_name(idx);

    for (int i = 0; i < s_nw; i++) {
        widget_t *w = &s_w[i];
        if (w->sig == idx) {
            widget_update(w);
        } else if (w->type == W_CHART && !w->timer) {
            for (int k = 0; k < w->nser; k++) {
                if (w->ser_sig[k] == idx) {
                    chart_push(w, idx);
                }
            }
        } else if (w->log && log_accepts(w->log, name)) {
            char line[176];
            snprintf(line, sizeof(line), "%s = %s", name, txt);
            log_append(w, line);
        }
    }
    rules_eval(idx);

    if (idx == s_sig_tab && s_tabview) {
        double v;
        if (sig_get_num(idx, &v) && v >= 0 && v < lv_tabview_get_tab_count(s_tabview) &&
                (uint32_t)v != lv_tabview_get_tab_active(s_tabview)) {
            lv_tabview_set_active(s_tabview, (uint32_t)v, LV_ANIM_OFF);
        }
    } else if (idx == s_sig_backlight) {
        double v;
        if (sig_get_num(idx, &v)) {
            if (v == 0) {
                screen_off();
            } else if (s_screen_off) {
                wake_cb(NULL);
            }
        }
    }

    if (s_set.ws_signals) {
        if (!ctx->batch) {
            ctx->batch = cJSON_CreateObject();
        }
        sig_add_json(ctx->batch, idx);
    }
    io_mqtt_publish_signal(name, txt);
}

static void tick_cb(lv_timer_t *t)
{
    tick_ctx_t ctx = {0};
    sig_take_dirty(on_dirty, &ctx);
    for (int i = 0; i < s_nw; i++) {
        if (s_w[i].log) {
            log_flush(&s_w[i]);
        }
    }
    if (ctx.batch) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddItemToObject(msg, "signals", ctx.batch);
        char *s = cJSON_PrintUnformatted(msg);
        if (s) {
            web_ws_broadcast(s);
            cJSON_free(s);
        }
        cJSON_Delete(msg);
    }
    if (s_set.screensaver_s > 0 && !s_screen_off &&
            lv_display_get_inactive_time(s_disp) > (uint32_t)s_set.screensaver_s * 1000) {
        screen_off();
    }
}

void ui_engine_init(lv_display_t *disp)
{
    s_disp = disp;
    s_w = heap_caps_calloc(MAX_WIDGETS, sizeof(widget_t), MALLOC_CAP_SPIRAM);
    if (!s_w) {
        s_w = calloc(MAX_WIDGETS, sizeof(widget_t));
    }
    s_sig_tab = sig_index("ui.tab", true);
    s_sig_backlight = sig_index("ui.backlight", true);
    sig_set_num_idx(s_sig_backlight, 1, false);
    lv_timer_create(tick_cb, TICK_MS, NULL);
}
