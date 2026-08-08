/*
 * json_min.h
 * -----------------------------------------------------------------------
 * Deliberately tiny JSON helpers.
 */

#ifndef JSON_MIN_H
#define JSON_MIN_H

#include <stddef.h>

/* Extracts the string value of "key" from a flat JSON object string,
 * e.g. json_min_get_string("{\"cmd\":\"reboot\"}", "cmd", buf, sizeof(buf))
 * writes "reboot" into buf. Returns 0 on success, -1 if key not found. */
int json_min_get_string(const char *json, const char *key, char *out, size_t out_sz);

/* Extracts an integer value, e.g. {"enabled": 1} */
int json_min_get_int(const char *json, const char *key, int *out);

#endif /* JSON_MIN_H */
