/*
 * Signal store: named values shared by inputs, widgets, rules and the network.
 *
 * Any task may set a signal. Changes are marked dirty and consumed by the UI
 * task (sig_take_dirty), which updates bound widgets, runs rules and pushes
 * updates to WebSocket clients.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#define SIG_NAME_MAX 32
#define SIG_MAX      512
/* Slots that names chosen by remote peers (WebSocket, REST, serial kv/json) cannot use. */
#define SIG_RESERVED 128

void sig_init(void);

/* Index of a signal, creating it if `create`. Returns -1 if missing or the table is full. */
int sig_index(const char *name, bool create);
const char *sig_name(int idx);

/*
 * Setters. `notify_always` marks the signal dirty even if the value did not
 * change (use for message-like sources such as serial lines).
 */
void sig_set_num(const char *name, double v, bool notify_always);
void sig_set_text(const char *name, const char *text, bool notify_always); /* numbers are detected */
void sig_set_num_idx(int idx, double v, bool notify_always);

/*
 * For names that come from the network or a serial peer: same as above, but a
 * new signal is only created while more than SIG_RESERVED slots are free, so
 * a chatty device cannot fill the table the layout needs.
 */
void sig_set_num_remote(const char *name, double v);
void sig_set_text_remote(const char *name, const char *text);

int sig_count(void);

/* Numeric value (strings parse as numbers, "on"/"true" = 1). False if never set. */
bool sig_get_num(int idx, double *out);
/* Text value: strings as-is, numbers compactly formatted. Returns false if never set. */
bool sig_get_text(int idx, char *buf, size_t len);
bool sig_is_text(int idx);

/* Call fn for every signal changed since the last call (consumer: UI task). */
typedef void (*sig_visit_fn)(int idx, void *ctx);
void sig_take_dirty(sig_visit_fn fn, void *ctx);

/* Add {"name": value, ...} for all signals that have a value. */
void sig_to_json(cJSON *obj);
/* Add one signal to obj under its name. */
void sig_add_json(cJSON *obj, int idx);
