#include "json_min.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Finds "key" in json and returns a pointer just past the following
 * colon, skipping whitespace. Returns NULL if not found. */
static const char *find_value_start(const char *json, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;

    pos += strlen(pattern);
    const char *colon = strchr(pos, ':');
    if (!colon) return NULL;
    colon++;
    while (*colon == ' ' || *colon == '\t' || *colon == '\n') colon++;
    return colon;
}

int json_min_get_string(const char *json, const char *key, char *out, size_t out_sz)
{
    const char *val = find_value_start(json, key);
    if (!val) return -1;
    if (*val != '"') return -1;
    val++;

    size_t i = 0;
    while (*val && *val != '"' && i + 1 < out_sz) {
        out[i++] = *val++;
    }
    out[i] = '\0';
    return 0;
}

int json_min_get_int(const char *json, const char *key, int *out)
{
    const char *val = find_value_start(json, key);
    if (!val) return -1;
    char *endptr;
    long v = strtol(val, &endptr, 10);
    if (endptr == val) return -1;
    *out = (int)v;
    return 0;
}
