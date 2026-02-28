#ifndef STRATA_JSON_UTIL_H
#define STRATA_JSON_UTIL_H

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Minimal JSON field extraction — finds "key":"value" and copies value.
 * Handles escaped quotes (\") inside the value. Un-escapes \" → " and \\ → \. */
static inline int json_get_string(const char *json, const char *key,
                                   char *out, int out_cap) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    start += strlen(pattern);

    /* Scan for closing quote, skipping escaped characters */
    int pos = 0;
    const char *p = start;
    while (*p && pos < out_cap - 1) {
        if (*p == '\\' && *(p + 1)) {
            /* Escaped character — un-escape common sequences */
            p++;
            if (*p == '"') out[pos++] = '"';
            else if (*p == '\\') out[pos++] = '\\';
            else if (*p == 'n') out[pos++] = '\n';
            else if (*p == 't') out[pos++] = '\t';
            else { out[pos++] = '\\'; if (pos < out_cap - 1) out[pos++] = *p; }
            p++;
        } else if (*p == '"') {
            break;  /* unescaped quote = end of string */
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
    return pos;
}

/* Escape a string for safe embedding in a JSON "value". Writes to out. */
static inline int json_escape(const char *src, int src_len, char *out, int out_cap) {
    int pos = 0;
    for (int i = 0; i < src_len && pos < out_cap - 2; i++) {
        if (src[i] == '"') { out[pos++] = '\\'; out[pos++] = '"'; }
        else if (src[i] == '\\') { out[pos++] = '\\'; out[pos++] = '\\'; }
        else if (src[i] == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (src[i] == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else out[pos++] = src[i];
    }
    out[pos] = '\0';
    return pos;
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
