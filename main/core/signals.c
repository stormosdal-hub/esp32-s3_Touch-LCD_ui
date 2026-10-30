#include "signals.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "util.h"

static const char *TAG = "signals";

typedef struct {
    char name[SIG_NAME_MAX];
    bool used;
    bool has_value;
    bool is_text;
    double num;
    char *text;
} signal_t;

static signal_t *s_sig;        /* SIG_MAX entries, in PSRAM */
static int s_count;
static uint32_t s_dirty[(SIG_MAX + 31) / 32];
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

void sig_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_sig = heap_caps_calloc(SIG_MAX, sizeof(signal_t), MALLOC_CAP_SPIRAM);
    if (!s_sig) {
        s_sig = calloc(SIG_MAX, sizeof(signal_t));
    }
}

int sig_count(void)
{
    return s_count;
}

static bool remote_may_create(const char *name)
{
    return sig_index(name, false) >= 0 || s_count < SIG_MAX - SIG_RESERVED;
}

void sig_set_num_remote(const char *name, double v)
{
    if (remote_may_create(name)) {
        sig_set_num(name, v, true);
    }
}

void sig_set_text_remote(const char *name, const char *text)
{
    if (remote_may_create(name)) {
        sig_set_text(name, text, true);
    }
}

static int find_locked(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_sig[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static int index_locked(const char *name, bool create)
{
    if (!name || !*name) {
        return -1;
    }
    int idx = find_locked(name);
    if (idx >= 0 || !create) {
        return idx;
    }
    if (s_count >= SIG_MAX) {
        ESP_LOGW(TAG, "signal table full, dropping '%s'", name);
        return -1;
    }
    idx = s_count++;
    signal_t *s = &s_sig[idx];
    memset(s, 0, sizeof(*s));
    strlcpy(s->name, name, sizeof(s->name));
    s->used = true;
    return idx;
}

int sig_index(const char *name, bool create)
{
    LOCK();
    int idx = index_locked(name, create);
    UNLOCK();
    return idx;
}

const char *sig_name(int idx)
{
    /* Names are never removed or changed once created, so no lock needed. */
    return (idx >= 0 && idx < s_count) ? s_sig[idx].name : "";
}

static void mark_dirty(int idx)
{
    s_dirty[idx / 32] |= 1u << (idx % 32);
}

static void set_num_locked(int idx, double v, bool notify_always)
{
    signal_t *s = &s_sig[idx];
    bool changed = !s->has_value || s->is_text || s->num != v;
    s->num = v;
    s->is_text = false;
    s->has_value = true;
    free(s->text);
    s->text = NULL;
    if (changed || notify_always) {
        mark_dirty(idx);
    }
}

void sig_set_num_idx(int idx, double v, bool notify_always)
{
    if (idx < 0) {
        return;
    }
    LOCK();
    if (idx < s_count) {
        set_num_locked(idx, v, notify_always);
    }
    UNLOCK();
}

void sig_set_num(const char *name, double v, bool notify_always)
{
    LOCK();
    int idx = index_locked(name, true);
    if (idx >= 0) {
        set_num_locked(idx, v, notify_always);
    }
    UNLOCK();
}

void sig_set_text(const char *name, const char *text, bool notify_always)
{
    if (!text) {
        text = "";
    }
    double v;
    if (util_parse_number(text, &v, false)) {
        sig_set_num(name, v, notify_always);
        return;
    }
    LOCK();
    int idx = index_locked(name, true);
    if (idx >= 0) {
        signal_t *s = &s_sig[idx];
        bool changed = !s->has_value || !s->is_text || !s->text || strcmp(s->text, text) != 0;
        if (changed) {
            char *copy = strdup(text);
            if (copy) {
                free(s->text);
                s->text = copy;
                s->is_text = true;
                s->has_value = true;
                if (!util_parse_number(text, &s->num, true)) {
                    s->num = 0;
                }
            }
        }
        if (changed || notify_always) {
            mark_dirty(idx);
        }
    }
    UNLOCK();
}

bool sig_get_num(int idx, double *out)
{
    bool ok = false;
    LOCK();
    if (idx >= 0 && idx < s_count && s_sig[idx].has_value) {
        *out = s_sig[idx].num;
        ok = true;
    }
    UNLOCK();
    return ok;
}

bool sig_get_text(int idx, char *buf, size_t len)
{
    bool ok = false;
    LOCK();
    if (idx >= 0 && idx < s_count && s_sig[idx].has_value) {
        if (s_sig[idx].is_text) {
            strlcpy(buf, s_sig[idx].text, len);
        } else {
            util_num_to_str(s_sig[idx].num, buf, len);
        }
        ok = true;
    }
    UNLOCK();
    if (!ok && len) {
        buf[0] = '\0';
    }
    return ok;
}

bool sig_is_text(int idx)
{
    bool r = false;
    LOCK();
    if (idx >= 0 && idx < s_count) {
        r = s_sig[idx].is_text;
    }
    UNLOCK();
    return r;
}

void sig_take_dirty(sig_visit_fn fn, void *ctx)
{
    uint32_t dirty[sizeof(s_dirty) / sizeof(s_dirty[0])];
    LOCK();
    memcpy(dirty, s_dirty, sizeof(dirty));
    memset(s_dirty, 0, sizeof(s_dirty));
    UNLOCK();
    for (int w = 0; w < (int)(sizeof(dirty) / sizeof(dirty[0])); w++) {
        while (dirty[w]) {
            int b = __builtin_ctz(dirty[w]);
            dirty[w] &= dirty[w] - 1;
            fn(w * 32 + b, ctx);
        }
    }
}

static void add_locked(cJSON *obj, int idx)
{
    signal_t *s = &s_sig[idx];
    if (!s->has_value) {
        return;
    }
    if (s->is_text) {
        cJSON_AddStringToObject(obj, s->name, s->text);
    } else {
        cJSON_AddNumberToObject(obj, s->name, s->num);
    }
}

void sig_to_json(cJSON *obj)
{
    LOCK();
    for (int i = 0; i < s_count; i++) {
        add_locked(obj, i);
    }
    UNLOCK();
}

void sig_add_json(cJSON *obj, int idx)
{
    LOCK();
    if (idx >= 0 && idx < s_count) {
        add_locked(obj, idx);
    }
    UNLOCK();
}
