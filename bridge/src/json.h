/* Tiny helpers for the flat JSON objects exchanged with the TV app. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Appends a JSON string literal (with quotes) for s into out. Returns bytes written. */
size_t json_escape(char *out, size_t outlen, const char *s);
/* Looks up a top-level key in a flat object. */
bool json_get_str(const char *obj, const char *key, char *out, size_t outlen);
long json_get_int(const char *obj, const char *key, long def);
double json_get_num(const char *obj, const char *key, double def);
