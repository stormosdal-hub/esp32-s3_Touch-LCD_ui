/*
 * Action worker. Widget events and rules queue actions here; they run in a
 * separate task so slow I/O (HTTP, RS-485, delays) never blocks the UI.
 */
#pragma once

#include <stdbool.h>
#include "cJSON.h"

void actions_init(void);

/*
 * Queue every action in `actions` (a JSON array). The triggering value is
 * `num`, or `text` if non-NULL (e.g. dropdown option text or a typed string).
 * `source` is the widget or rule id, available as {id} in templates.
 */
void actions_run(const cJSON *actions, double num, const char *text, bool value_is_text, const char *source);
