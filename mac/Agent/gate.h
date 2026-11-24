#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns 1 when `needle` is the application that owns the frontmost window,
 * 0 otherwise. `needle` is matched against the bundle identifier first and the
 * localized application name second, both case-insensitively. A NULL or empty
 * needle disables the gate and always returns 1.
 *
 * An application that is not running cannot own the frontmost window, so this
 * single check covers both "is it running" and "does it have focus".
 */
int tp_gate_allows(const char *needle);

/*
 * Same decision as tp_gate_allows, but also writes a short description of the
 * frontmost application into `out` (name, bundle identifier, active flag) so a
 * log line can explain why the gate opened or closed.
 */
int tp_gate_status(const char *needle, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
