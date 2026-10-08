// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The newest published release (release_check.h). GitHub lists releases newest first; drafts are not shown to an anonymous
// caller. Its rate limit for anonymous calls (60 an hour per address) is far above one call per start.
#include "release_check.h"
#include "diagnostics.h"
#include "platform.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool releaseNewest(const char *url, char *tag, size_t size) {
    static const PlatformHttpHeader headers[] = {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    char error[200];
    int status = 0;
    tag[0] = 0;
    PlatformHttp *h = platformHttpSend("GET", url, headers, 2, NULL, 0, &status, error, sizeof(error));
    if (!h) {
        diagnosticsTrace("release check: %s", error);
        return false;
    }
    static char text[256 * 1024];
    size_t have = 0;
    for (int64_t got; have < sizeof(text) - 1 && (got = platformHttpRead(h, text + have, sizeof(text) - 1 - have)) > 0;) have += (size_t)got;
    text[have] = 0;
    platformHttpClose(h);
    if (status != 200) {
        diagnosticsTrace("release check: GitHub answered HTTP %d", status);
        return false;
    }
    const char *p = strstr(text, "\"tag_name\"");  // the first release in the list is the newest
    if (p) p = strchr(p + 10, ':');
    if (p) p = strchr(p, '"');
    if (!p) return false;
    size_t n = strcspn(p + 1, "\"");
    if (!n || n >= size) return false;
    memcpy(tag, p + 1, n);
    tag[n] = 0;
    return true;
}

// One dot-separated piece: numbers as numbers, words as text, numbers before words (as semantic versioning orders them).
static int comparePiece(const char *a, size_t a_length, const char *b, size_t b_length) {
    bool a_number = a_length && strspn(a, "0123456789") >= a_length, b_number = b_length && strspn(b, "0123456789") >= b_length;
    if (a_number && b_number) {
        unsigned long long x = strtoull(a, NULL, 10), y = strtoull(b, NULL, 10);
        return x < y ? -1 : x > y;
    }
    if (a_number != b_number) return a_number ? -1 : 1;
    size_t n = a_length < b_length ? a_length : b_length;
    int c = strncmp(a, b, n);
    return c ? (c < 0 ? -1 : 1) : (a_length < b_length ? -1 : a_length > b_length);
}
static int compareDotted(const char *a, const char *a_end, const char *b, const char *b_end) {
    while (a < a_end || b < b_end) {
        if (a >= a_end) return -1;  // fewer pieces: older ("beta" < "beta.2")
        if (b >= b_end) return 1;
        const char *a_dot = memchr(a, '.', (size_t)(a_end - a)), *b_dot = memchr(b, '.', (size_t)(b_end - b));
        size_t a_length = (size_t)((a_dot ? a_dot : a_end) - a), b_length = (size_t)((b_dot ? b_dot : b_end) - b);
        int c = comparePiece(a, a_length, b, b_length);
        if (c) return c;
        a += a_length + (a_dot != NULL);
        b += b_length + (b_dot != NULL);
    }
    return 0;
}
int releaseCompare(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') ++a;
    if (*b == 'v' || *b == 'V') ++b;
    const char *a_label = strchr(a, '-'), *b_label = strchr(b, '-');
    const char *a_core = a_label ? a_label : a + strlen(a), *b_core = b_label ? b_label : b + strlen(b);
    int c = compareDotted(a, a_core, b, b_core);
    if (c) return c;
    if (!a_label || !b_label) return a_label ? -1 : b_label ? 1 : 0;  // a final release is newer than its pre-releases
    return compareDotted(a_label + 1, a_label + strlen(a_label), b_label + 1, b_label + strlen(b_label));
}
