#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t json_escape(char *out, size_t outlen, const char *s) {
    size_t o = 0;
#define PUT(c) do { if (o + 1 < outlen) out[o] = (c); o++; } while (0)
    PUT('"');
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
        case '"': PUT('\\'); PUT('"'); break;
        case '\\': PUT('\\'); PUT('\\'); break;
        case '\n': PUT('\\'); PUT('n'); break;
        case '\r': PUT('\\'); PUT('r'); break;
        case '\t': PUT('\\'); PUT('t'); break;
        default:
            if (*p < 0x20) {
                char u[8];
                snprintf(u, sizeof(u), "\\u%04x", *p);
                for (char *q = u; *q; q++) PUT(*q);
            } else {
                PUT((char)*p);
            }
        }
    }
    PUT('"');
#undef PUT
    if (outlen) out[o < outlen ? o : outlen - 1] = 0;
    return o;
}

/* Returns a pointer to the first character of the value for key, or NULL. */
static const char *find_value(const char *obj, const char *key) {
    size_t klen = strlen(key);
    const char *p = obj;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                return q;
            }
        }
        /* Skip the rest of this string literal. */
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        if (*p) p++;
    }
    return NULL;
}

bool json_get_str(const char *obj, const char *key, char *out, size_t outlen) {
    const char *v = find_value(obj, key);
    if (!v || *v != '"' || outlen == 0) return false;
    v++;
    size_t o = 0;
    while (*v && *v != '"') {
        char c = *v++;
        if (c == '\\' && *v) {
            char e = *v++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'u': {
                /* Only ASCII escapes are expected from the TV app. */
                char hex[5] = {0};
                for (int i = 0; i < 4 && *v; i++) hex[i] = *v++;
                c = (char)strtol(hex, NULL, 16);
                break;
            }
            default: c = e;
            }
        }
        if (o + 1 < outlen) out[o++] = c;
    }
    out[o] = 0;
    return true;
}

double json_get_num(const char *obj, const char *key, double def) {
    const char *v = find_value(obj, key);
    if (!v) return def;
    if (strncmp(v, "true", 4) == 0) return 1;
    if (strncmp(v, "false", 5) == 0) return 0;
    char *end;
    double d = strtod(v, &end);
    return end == v ? def : d;
}

long json_get_int(const char *obj, const char *key, long def) {
    double d = json_get_num(obj, key, (double)def);
    return (long)d;
}
