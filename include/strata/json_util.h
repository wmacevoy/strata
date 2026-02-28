#ifndef STRATA_JSON_UTIL_H
#define STRATA_JSON_UTIL_H

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Minimal JSON field extraction — finds "key":"value" and copies value */
static inline int json_get_string(const char *json, const char *key,
                                   char *out, int out_cap) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) return -1;
    int len = (int)(end - start);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return len;
}

/* Extract a JSON array of strings: "key":["a","b"] */
static inline int json_get_string_array(const char *json, const char *key,
                                         char **out, int max_items) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
    const char *start = strstr(json, pattern);
    if (!start) return 0;
    start += strlen(pattern);
    const char *end = strchr(start, ']');
    if (!end) return 0;

    int count = 0;
    const char *p = start;
    while (p < end && count < max_items) {
        const char *q = strchr(p, '"');
        if (!q || q >= end) break;
        q++;
        const char *r = strchr(q, '"');
        if (!r || r >= end) break;
        int len = (int)(r - q);
        out[count] = malloc(len + 1);
        memcpy(out[count], q, len);
        out[count][len] = '\0';
        count++;
        p = r + 1;
    }
    return count;
}

/* Extract a JSON integer: "key":123 */
static inline int json_get_int(const char *json, const char *key, int *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    start += strlen(pattern);
    *out = atoi(start);
    return 0;
}

#endif
