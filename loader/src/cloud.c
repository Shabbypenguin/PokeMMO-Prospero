// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// Google Drive backup (cloud.h). Plain REST over the platform's HTTPS (platformHttpSend): OAuth 2.0 for TV and limited-input
// devices, Drive v3 (files.list, files.create, uploads: multipart for settings, resumable in 8 MiB pieces for ROMs, alt=media
// downloads, files.delete). JSON answers are read with a small scanner (only Google's own, well-formed answers are read).
// PROSPERO_CLOUD_BASE (PC tests only) points every address at a stand-in server (tools/cloud_server.py).
#include "cloud.h"
#include "diagnostics.h"
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// The project's Google OAuth client ("TVs and Limited Input devices"). Google treats this type's secret as public: it ships in
// every copy, and it reaches nobody's files without that person's approval on their phone.
#define GOOGLE_CLIENT_ID "175729512800-i8dpmc29v1k0up2rg6o974rifrs70nl5.apps.googleusercontent.com"
#define GOOGLE_CLIENT_SECRET "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"
#define GOOGLE_SCOPE "https://www.googleapis.com/auth/drive.file"
#define FOLDER_MIME "application/vnd.google-apps.folder"
#define TOP_FOLDER "PokeMMO Prospero"
#define CHUNK (8u << 20)  // resumable upload piece: a multiple of 256 KiB

static CloudStatus status;
static char state[256], token_path[300], declined_path[300];
static char access_token[2048];
static uint64_t access_expires_ns;
static char top_id[128], roms_id[128];
static char oauth_base[200] = "https://oauth2.googleapis.com", api_base[200] = "https://www.googleapis.com/drive/v3",
            upload_base[200] = "https://www.googleapis.com/upload/drive/v3";

static void setError(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void setError(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(status.error, sizeof(status.error), format, args);
    va_end(args);
    diagnosticsTrace("cloud: %s", status.error);
}
static void setActivity(const char *text, uint64_t total) {
    snprintf(status.activity, sizeof(status.activity), "%s", text);
    atomic_store(&status.done, 0);
    atomic_store(&status.total, total);
}

// ---- small helpers -------------------------------------------------------------------------------------------------------------
static bool readFile(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t got = read(fd, out, size - 1);
    close(fd);
    out[got > 0 ? got : 0] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return got > 0;
}
static bool writeFile(const char *path, const void *data, size_t size) {
    char temporary[400];
    snprintf(temporary, sizeof(temporary), "%s.part", path);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, data, size) == (ssize_t)size;
    close(fd);
    if (!ok || rename(temporary, path)) {
        unlink(temporary);
        return false;
    }
    return true;
}
static void makeFolders(const char *path) {
    char partial[512];
    snprintf(partial, sizeof(partial), "%s", path);
    for (char *slash = strchr(partial + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        mkdir(partial, 0755);
        *slash = '/';
    }
    mkdir(partial, 0755);
}
static void urlEncode(const char *in, char *out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 4 < size; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p))
            out[n++] = (char)*p;
        else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = 0;
}
// A string for a JSON body or a Drive query (quotes and backslashes escaped).
static void quoteEscape(const char *in, char *out, size_t size, char quote) {
    size_t n = 0;
    for (const char *p = in; *p && n + 3 < size; ++p) {
        if (*p == quote || *p == '\\') out[n++] = '\\';
        out[n++] = *p;
    }
    out[n] = 0;
}

// ---- JSON (Google's answers): the value after "key": at any depth, read as a string or a number -------------------------------------
static const char *skipString(const char *p) {  // p at the opening quote; returns past the closing one
    for (++p; *p && *p != '"'; ++p)
        if (*p == '\\' && p[1]) ++p;
    return *p ? p + 1 : p;
}
static const char *jsonFind(const char *json, const char *end, const char *key) {
    size_t key_length = strlen(key);
    for (const char *p = json; p && p < end && *p; ++p) {
        if (*p != '"') continue;
        const char *after = skipString(p);
        if ((size_t)(after - p - 2) == key_length && !strncmp(p + 1, key, key_length)) {
            const char *q = after;
            while (q < end && (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) ++q;
            if (q < end && *q == ':') {
                ++q;
                while (q < end && (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) ++q;
                return q;
            }
        }
        p = after - 1;
    }
    return NULL;
}
static bool jsonString(const char *json, const char *end, const char *key, char *out, size_t size) {
    const char *p = jsonFind(json, end, key);
    out[0] = 0;
    if (!p) return false;
    if (*p != '"') {  // a number or a word: up to the next separator
        size_t n = strcspn(p, ",}] \r\n");
        snprintf(out, size, "%.*s", (int)n, p);
        return n > 0;
    }
    size_t n = 0;
    for (++p; p < end && *p && *p != '"' && n + 1 < size; ++p) {
        if (*p == '\\' && p[1]) {
            ++p;
            char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'u' ? '?' : *p;
            if (*p == 'u') p += 4 <= strlen(p) ? 4 : 0;
            out[n++] = c;
        } else
            out[n++] = *p;
    }
    out[n] = 0;
    return true;
}
// The objects of the array "files": calls `each` with the bounds of every object.
static unsigned jsonEachFile(const char *json, const char *end, void (*each)(const char *, const char *, void *), void *context) {
    const char *p = jsonFind(json, end, "files");
    if (!p || *p != '[') return 0;
    unsigned count = 0;
    int depth = 0;
    const char *start = NULL;
    for (++p; p < end && *p; ++p) {
        if (*p == '"') {
            p = skipString(p) - 1;
            continue;
        }
        if (*p == '{') {
            if (depth++ == 0) start = p;
        } else if (*p == '}') {
            if (--depth == 0 && start) {
                each(start, p + 1, context);
                ++count;
            }
        } else if (*p == ']' && depth == 0)
            break;
    }
    return count;
}

// ---- HTTP ------------------------------------------------------------------------------------------------------------------------------
// A request whose whole answer (JSON, at most 4 MiB) is wanted. Returns the status (0: no answer); *response is malloc'd.
static int call(const char *method, const char *url, const PlatformHttpHeader *headers, unsigned count, const void *body, size_t size, char **response,
                char *location, size_t location_size) {
    char error[200];
    int code = 0;
    if (response) *response = NULL;
    PlatformHttp *h = platformHttpSend(method, url, headers, count, body, size, &code, error, sizeof(error));
    if (!h) {
        setError("%s", error);
        return 0;
    }
    if (location) {
        location[0] = 0;
        platformHttpHeader(h, "Location", location, location_size);
    }
    size_t capacity = 65536, have = 0;
    char *text = malloc(capacity);
    for (int64_t got; text && (got = platformHttpRead(h, text + have, capacity - have - 1)) > 0;) {
        have += (size_t)got;
        if (capacity - have < 2) {
            if (capacity >= (4u << 20)) break;
            char *bigger = realloc(text, capacity * 2);
            if (!bigger) break;
            text = bigger;
            capacity *= 2;
        }
    }
    if (text) text[have] = 0;
    platformHttpClose(h);
    if (response)
        *response = text;
    else
        free(text);
    return code;
}

// ---- sign-in and tokens ------------------------------------------------------------------------------------------------------------
void cloudInit(const char *state_folder) {
    snprintf(state, sizeof(state), "%s", state_folder);
    makeFolders(state);
    snprintf(token_path, sizeof(token_path), "%s/google-token", state);
    snprintf(declined_path, sizeof(declined_path), "%s/declined", state);
    const char *base = getenv("PROSPERO_CLOUD_BASE");
    if (base && *base) {
        snprintf(oauth_base, sizeof(oauth_base), "%s", base);
        snprintf(api_base, sizeof(api_base), "%s/drive/v3", base);
        snprintf(upload_base, sizeof(upload_base), "%s/upload/drive/v3", base);
    }
}
CloudStatus *cloudStatus(void) { return &status; }
bool cloudSignedIn(void) {
    struct stat info;
    return !stat(token_path, &info) && info.st_size > 0;
}
bool cloudDeclined(void) {
    struct stat info;
    return !stat(declined_path, &info);
}
void cloudDecline(void) { writeFile(declined_path, "1\n", 2); }
void cloudSignOut(void) {
    unlink(token_path);
    access_token[0] = 0;
    top_id[0] = roms_id[0] = 0;
}

static const PlatformHttpHeader form_header[] = {{"Content-Type", "application/x-www-form-urlencoded"}};
static bool takeTokens(const char *json, bool keep_refresh) {
    const char *end = json + strlen(json);
    char value[2048], seconds[32];
    if (!jsonString(json, end, "access_token", value, sizeof(value)) || !value[0]) return false;
    snprintf(access_token, sizeof(access_token), "%s", value);
    jsonString(json, end, "expires_in", seconds, sizeof(seconds));
    long lifetime = atol(seconds);
    access_expires_ns = platformMonotonicNs() + (uint64_t)(lifetime > 120 ? lifetime - 60 : 60) * 1000000000ull;
    if (keep_refresh && jsonString(json, end, "refresh_token", value, sizeof(value)) && value[0]) {
        char line[2100];
        int length = snprintf(line, sizeof(line), "%s\n", value);
        if (!writeFile(token_path, line, (size_t)length)) diagnosticsTrace("cloud: the sign-in could not be stored");
    }
    return true;
}
static bool accessToken(void) {
    if (access_token[0] && platformMonotonicNs() < access_expires_ns) return true;
    char refresh[2048], body[2600], encoded[2100], url[300];
    if (!readFile(token_path, refresh, sizeof(refresh)) || !refresh[0]) {
        setError("not signed in to Google Drive");
        return false;
    }
    urlEncode(refresh, encoded, sizeof(encoded));
    int length = snprintf(body, sizeof(body), "client_id=%s&client_secret=%s&grant_type=refresh_token&refresh_token=%s", GOOGLE_CLIENT_ID,
                          GOOGLE_CLIENT_SECRET, encoded);
    snprintf(url, sizeof(url), "%s/token", oauth_base);
    char *answer = NULL;
    int code = call("POST", url, form_header, 1, body, (size_t)length, &answer, NULL, 0);
    bool ok = code == 200 && answer && takeTokens(answer, false);
    if (!ok) {
        char reason[64] = "";
        if (answer) jsonString(answer, answer + strlen(answer), "error", reason, sizeof(reason));
        setError("Google did not renew the sign-in (HTTP %d %s)", code, reason);
        if (code == 400 && !strcmp(reason, "invalid_grant")) cloudSignOut();  // revoked or expired: sign in again
    }
    free(answer);
    return ok;
}
bool cloudSignIn(_Atomic bool *cancel) {
    char url[300], body[400], *answer = NULL;
    snprintf(url, sizeof(url), "%s/device/code", oauth_base);
    int length = snprintf(body, sizeof(body), "client_id=%s&scope=%s", GOOGLE_CLIENT_ID, "https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdrive.file");
    atomic_store(&status.phase, CLOUD_BUSY);
    setActivity("Contacting Google", 0);
    int code = call("POST", url, form_header, 1, body, (size_t)length, &answer, NULL, 0);
    char device[512] = "", interval_text[16] = "", expires_text[16] = "";
    if (code == 200 && answer) {
        const char *end = answer + strlen(answer);
        jsonString(answer, end, "device_code", device, sizeof(device));
        jsonString(answer, end, "user_code", status.user_code, sizeof(status.user_code));
        if (!jsonString(answer, end, "verification_url", status.verify_url, sizeof(status.verify_url)))
            jsonString(answer, end, "verification_uri", status.verify_url, sizeof(status.verify_url));
        jsonString(answer, end, "interval", interval_text, sizeof(interval_text));
        jsonString(answer, end, "expires_in", expires_text, sizeof(expires_text));
    }
    if (!device[0] || !status.user_code[0]) {
        char reason[96] = "";
        if (answer) jsonString(answer, answer + strlen(answer), "error", reason, sizeof(reason));
        setError("Google did not start the sign-in (HTTP %d %s)", code, reason);
        free(answer);
        atomic_store(&status.phase, CLOUD_FAILED);
        return false;
    }
    free(answer);
    diagnosticsTrace("cloud: sign-in code shown (%s at %s)", status.user_code, status.verify_url);
    atomic_store(&status.phase, CLOUD_CODE);
    unsigned interval = (unsigned)atoi(interval_text);
    if (interval < 2) interval = 5;
    uint64_t deadline = platformMonotonicNs() + (uint64_t)(atoi(expires_text) > 0 ? atoi(expires_text) : 900) * 1000000000ull;
    snprintf(url, sizeof(url), "%s/token", oauth_base);
    char encoded[600];
    urlEncode(device, encoded, sizeof(encoded));
    length = snprintf(body, sizeof(body), "client_id=%s&client_secret=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
                      GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, encoded);
    while (platformMonotonicNs() < deadline) {
        for (unsigned waited = 0; waited < interval * 10; ++waited) {
            if (cancel && atomic_load(cancel)) {
                atomic_store(&status.phase, CLOUD_IDLE);
                diagnosticsTrace("cloud: sign-in cancelled");
                return false;
            }
            platformSleepNs(100000000ull);
        }
        code = call("POST", url, form_header, 1, body, (size_t)length, &answer, NULL, 0);
        char reason[64] = "";
        if (answer) jsonString(answer, answer + strlen(answer), "error", reason, sizeof(reason));
        if (code == 200 && answer && takeTokens(answer, true)) {
            free(answer);
            unlink(declined_path);
            diagnosticsTrace("cloud: signed in to Google Drive");
            atomic_store(&status.phase, CLOUD_DONE);
            return true;
        }
        free(answer);
        if (!strcmp(reason, "slow_down")) interval += 5;
        else if (strcmp(reason, "authorization_pending")) {
            setError(!strcmp(reason, "access_denied") ? "the sign-in was declined on the phone" : "Google refused the sign-in (HTTP %d %s)", code, reason);
            atomic_store(&status.phase, CLOUD_FAILED);
            return false;
        }
    }
    setError("the code expired before it was entered");
    atomic_store(&status.phase, CLOUD_FAILED);
    return false;
}

// ---- Drive ------------------------------------------------------------------------------------------------------------------------
typedef struct {
    char name[256], id[128];
    long long size;
    bool folder;
} RemoteFile;
typedef struct {
    RemoteFile *files;
    unsigned count, capacity;
} RemoteList;
static void addFile(const char *start, const char *end, void *context) {
    RemoteList *list = context;
    if (list->count == list->capacity) {
        unsigned capacity = list->capacity ? list->capacity * 2 : 16;
        RemoteFile *bigger = realloc(list->files, capacity * sizeof(*bigger));
        if (!bigger) return;
        list->files = bigger;
        list->capacity = capacity;
    }
    RemoteFile *f = &list->files[list->count];
    memset(f, 0, sizeof(*f));
    char size[32] = "", mime[96] = "";
    jsonString(start, end, "name", f->name, sizeof(f->name));
    jsonString(start, end, "id", f->id, sizeof(f->id));
    jsonString(start, end, "size", size, sizeof(size));
    jsonString(start, end, "mimeType", mime, sizeof(mime));
    f->size = atoll(size);
    f->folder = !strcmp(mime, FOLDER_MIME);
    if (f->id[0]) ++list->count;
}
static int authorized(const char *method, const char *url, const char *content_type, const PlatformHttpHeader *more, unsigned more_count, const void *body,
                      size_t size, char **response, char *location, size_t location_size) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!accessToken()) return 0;
        char bearer[2100];
        snprintf(bearer, sizeof(bearer), "Bearer %s", access_token);
        PlatformHttpHeader headers[6] = {{"Authorization", bearer}};
        unsigned count = 1;
        if (content_type) headers[count++] = (PlatformHttpHeader){"Content-Type", content_type};
        for (unsigned i = 0; i < more_count && count < 6; ++i) headers[count++] = more[i];
        int code = call(method, url, headers, count, body, size, response, location, location_size);
        if (code != 401) return code;
        if (response) {
            free(*response);
            *response = NULL;
        }
        access_token[0] = 0;  // expired early: renew once
    }
    return 401;
}
// Files in a folder (parent "root" for the top), optionally only those with `name`.
static bool listFolder(const char *parent, const char *name, RemoteList *list) {
    memset(list, 0, sizeof(*list));
    char query[600], escaped[300], encoded[1800], url[3200], page[512] = "";
    if (name) {
        quoteEscape(name, escaped, sizeof(escaped), '\'');
        snprintf(query, sizeof(query), "'%s' in parents and trashed=false and name='%s'", parent, escaped);
    } else
        snprintf(query, sizeof(query), "'%s' in parents and trashed=false", parent);
    urlEncode(query, encoded, sizeof(encoded));
    do {
        char page_part[600] = "";
        if (page[0]) snprintf(page_part, sizeof(page_part), "&pageToken=%s", page);
        snprintf(url, sizeof(url), "%s/files?q=%s&fields=nextPageToken%%2Cfiles(id%%2Cname%%2Csize%%2CmimeType)&pageSize=1000&spaces=drive%s", api_base,
                 encoded, page_part);
        char *answer = NULL;
        int code = authorized("GET", url, NULL, NULL, 0, NULL, 0, &answer, NULL, 0);
        if (code != 200 || !answer) {
            if (code) setError("Drive did not list the files (HTTP %d)", code);
            free(answer);
            return false;
        }
        const char *end = answer + strlen(answer);
        jsonEachFile(answer, end, addFile, list);
        page[0] = 0;
        jsonString(answer, end, "nextPageToken", page, sizeof(page));
        free(answer);
    } while (page[0]);
    return true;
}
static bool makeFolder(const char *parent, const char *name, char *id, size_t id_size) {
    RemoteList list;
    if (!listFolder(parent, name, &list)) return false;
    for (unsigned i = 0; i < list.count; ++i)
        if (list.files[i].folder) {
            snprintf(id, id_size, "%s", list.files[i].id);
            free(list.files);
            return true;
        }
    free(list.files);
    char body[512], escaped[300], url[300], *answer = NULL;
    quoteEscape(name, escaped, sizeof(escaped), '"');
    int length = snprintf(body, sizeof(body), "{\"name\":\"%s\",\"mimeType\":\"" FOLDER_MIME "\",\"parents\":[\"%s\"]}", escaped, parent);
    snprintf(url, sizeof(url), "%s/files?fields=id", api_base);
    int code = authorized("POST", url, "application/json; charset=UTF-8", NULL, 0, body, (size_t)length, &answer, NULL, 0);
    bool ok = (code == 200 || code == 201) && answer && jsonString(answer, answer + strlen(answer), "id", id, id_size) && id[0];
    if (!ok) setError("Drive did not make the folder %s (HTTP %d)", name, code);
    free(answer);
    return ok;
}
static bool folders(void) {
    if (top_id[0] && roms_id[0]) return true;
    return makeFolder("root", TOP_FOLDER, top_id, sizeof(top_id)) && makeFolder(top_id, "roms", roms_id, sizeof(roms_id));
}
static bool deleteFile(const char *id) {
    char url[300];
    snprintf(url, sizeof(url), "%s/files/%s", api_base, id);
    int code = authorized("DELETE", url, NULL, NULL, 0, NULL, 0, NULL, NULL, 0);
    return code == 204 || code == 200 || code == 404;
}
// A small file in one request (multipart): the settings.
static bool uploadSmall(const char *parent, const char *name, const void *data, size_t size, char *id, size_t id_size) {
    static const char boundary[] = "prospero-c7d1f0a2b9";
    char head[800], escaped[300], url[300], tail[64];
    quoteEscape(name, escaped, sizeof(escaped), '"');
    int head_length = snprintf(head, sizeof(head),
                               "--%s\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{\"name\":\"%s\",\"parents\":[\"%s\"]}\r\n--%s\r\n"
                               "Content-Type: application/octet-stream\r\n\r\n",
                               boundary, escaped, parent, boundary);
    int tail_length = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
    size_t total = (size_t)head_length + size + (size_t)tail_length;
    char *body = malloc(total);
    if (!body) return false;
    memcpy(body, head, (size_t)head_length);
    memcpy(body + head_length, data, size);
    memcpy(body + head_length + size, tail, (size_t)tail_length);
    char type[96], *answer = NULL;
    snprintf(type, sizeof(type), "multipart/related; boundary=%s", boundary);
    snprintf(url, sizeof(url), "%s/files?uploadType=multipart&fields=id", upload_base);
    int code = authorized("POST", url, type, NULL, 0, body, total, &answer, NULL, 0);
    free(body);
    bool ok = (code == 200 || code == 201) && answer && jsonString(answer, answer + strlen(answer), "id", id, id_size);
    if (!ok) setError("Drive did not take %s (HTTP %d)", name, code);
    free(answer);
    return ok;
}
// A large file in pieces (resumable): a ROM. A piece that fails is sent again (three tries).
static bool uploadLarge(const char *parent, const char *name, const char *path) {
    int fd = open(path, O_RDONLY);
    struct stat info;
    if (fd < 0 || fstat(fd, &info)) {
        if (fd >= 0) close(fd);
        setError("%s cannot be read", name);
        return false;
    }
    long long total = info.st_size;
    char body[600], escaped[300], url[300], session[2048], length_text[32];
    quoteEscape(name, escaped, sizeof(escaped), '"');
    int length = snprintf(body, sizeof(body), "{\"name\":\"%s\",\"parents\":[\"%s\"]}", escaped, parent);
    snprintf(length_text, sizeof(length_text), "%lld", total);
    PlatformHttpHeader more[] = {{"X-Upload-Content-Type", "application/octet-stream"}, {"X-Upload-Content-Length", length_text}};
    snprintf(url, sizeof(url), "%s/files?uploadType=resumable", upload_base);
    int code = authorized("POST", url, "application/json; charset=UTF-8", more, 2, body, (size_t)length, NULL, session, sizeof(session));
    if (code != 200 || !session[0]) {
        close(fd);
        setError("Drive did not start the upload of %s (HTTP %d)", name, code);
        return false;
    }
    char *piece = malloc(CHUNK);
    bool ok = piece != NULL;
    long long offset = 0;
    while (ok && offset < total) {
        size_t want = total - offset < CHUNK ? (size_t)(total - offset) : CHUNK;
        ok = pread(fd, piece, want, offset) == (ssize_t)want;
        if (!ok) {
            setError("%s could not be read", name);
            break;
        }
        char range[96];
        snprintf(range, sizeof(range), "bytes %lld-%lld/%lld", offset, offset + (long long)want - 1, total);
        PlatformHttpHeader content_range = {"Content-Range", range};
        int tries = 0;
        do {
            code = authorized("PUT", session, "application/octet-stream", &content_range, 1, piece, want, NULL, NULL, 0);
        } while (code != 308 && code != 200 && code != 201 && ++tries < 3);
        ok = code == 308 || code == 200 || code == 201;
        if (!ok) setError("the upload of %s stopped (HTTP %d)", name, code);
        offset += (long long)want;
        atomic_fetch_add(&status.done, want);
    }
    free(piece);
    close(fd);
    return ok;
}
static bool download(const char *id, const char *path, long long size) {
    char url[300], temporary[600], error[200];
    snprintf(url, sizeof(url), "%s/files/%s?alt=media", api_base, id);
    snprintf(temporary, sizeof(temporary), "%s.part", path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!accessToken()) return false;
        char bearer[2100];
        snprintf(bearer, sizeof(bearer), "Bearer %s", access_token);
        PlatformHttpHeader auth = {"Authorization", bearer};
        int code = 0;
        PlatformHttp *h = platformHttpSend("GET", url, &auth, 1, NULL, 0, &code, error, sizeof(error));
        if (!h) {
            setError("%s", error);
            return false;
        }
        if (code == 401) {
            platformHttpClose(h);
            access_token[0] = 0;
            continue;
        }
        if (code != 200) {
            platformHttpClose(h);
            setError("Drive did not send a file (HTTP %d)", code);
            return false;
        }
        int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        static char buffer[1u << 20];
        long long have = 0;
        bool ok = fd >= 0;
        for (int64_t got; ok && (got = platformHttpRead(h, buffer, sizeof(buffer))) > 0;) {
            ok = write(fd, buffer, (size_t)got) == got;
            have += got;
            atomic_fetch_add(&status.done, (uint64_t)got);
        }
        platformHttpClose(h);
        if (fd >= 0) close(fd);
        ok = ok && (size < 0 || have == size) && !rename(temporary, path);
        if (!ok) {
            unlink(temporary);
            setError("a download stopped after %lld bytes", have);
        }
        return ok;
    }
    return false;
}

// ---- ROMs ------------------------------------------------------------------------------------------------------------------------------
static bool romName(const char *name) {
    size_t n = strlen(name);
    return n > 4 && name[0] != '.' && (!strcasecmp(name + n - 4, ".nds") || !strcasecmp(name + n - 4, ".gba"));
}
bool cloudSyncRoms(const char *rom_folder, bool upload, bool download_missing) {
    atomic_store(&status.phase, CLOUD_BUSY);
    setActivity("Checking Google Drive", 0);
    RemoteList remote;
    if (!folders() || !listFolder(roms_id, NULL, &remote)) {
        atomic_store(&status.phase, CLOUD_FAILED);
        return false;
    }
    bool ok = true;
    unsigned sent = 0, fetched = 0;
    if (download_missing) {
        uint64_t total = 0;
        for (unsigned i = 0; i < remote.count; ++i) {
            char path[600];
            struct stat info;
            snprintf(path, sizeof(path), "%s/%s", rom_folder, remote.files[i].name);
            if (!remote.files[i].folder && romName(remote.files[i].name) && !strchr(remote.files[i].name, '/') && stat(path, &info)) total += (uint64_t)remote.files[i].size;
        }
        setActivity("Restoring ROMs from Google Drive", total);
        for (unsigned i = 0; ok && i < remote.count; ++i) {
            const RemoteFile *f = &remote.files[i];
            char path[600];
            struct stat info;
            snprintf(path, sizeof(path), "%s/%s", rom_folder, f->name);
            if (f->folder || !romName(f->name) || strchr(f->name, '/') || !stat(path, &info)) continue;
            snprintf(status.activity, sizeof(status.activity), "Restoring %s", f->name);
            ok = download(f->id, path, f->size);
            fetched += ok;
            diagnosticsTrace("cloud: restored %s (%lld bytes)%s", f->name, f->size, ok ? "" : " FAILED");
        }
    }
    if (ok && upload) {
        int error = 0;
        PlatformDirectory *directory = platformDirectoryOpen(rom_folder, &error);
        char names[32][256];
        long long sizes[32];
        unsigned count = 0;
        uint64_t total = 0;
        if (directory) {
            char name[256];
            uint8_t type;
            uint64_t inode;
            while (platformDirectoryRead(directory, name, &type, &inode, &error) == 1 && count < 32) {
                if (type == 4 || !romName(name)) continue;
                char path[600];
                struct stat info;
                snprintf(path, sizeof(path), "%s/%s", rom_folder, name);
                if (stat(path, &info)) continue;
                bool there = false;
                for (unsigned i = 0; i < remote.count && !there; ++i) there = !strcmp(remote.files[i].name, name) && remote.files[i].size == info.st_size;
                if (there) continue;
                snprintf(names[count], sizeof(names[0]), "%s", name);
                sizes[count++] = info.st_size;
                total += (uint64_t)info.st_size;
            }
            platformDirectoryClose(directory);
        }
        setActivity("Backing up ROMs to Google Drive", total);
        for (unsigned i = 0; ok && i < count; ++i) {
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", rom_folder, names[i]);
            snprintf(status.activity, sizeof(status.activity), "Backing up %s", names[i]);
            ok = uploadLarge(roms_id, names[i], path);
            for (unsigned j = 0; ok && j < remote.count; ++j)  // the same name with another size: replaced once the new one is in
                if (!strcmp(remote.files[j].name, names[i])) deleteFile(remote.files[j].id);
            sent += ok;
            diagnosticsTrace("cloud: backed up %s (%lld bytes)%s", names[i], sizes[i], ok ? "" : " FAILED");
        }
    }
    free(remote.files);
    diagnosticsTrace("cloud: ROMs: %u restored, %u backed up%s", fetched, sent, ok ? "" : " (stopped by an error)");
    atomic_store(&status.phase, ok ? CLOUD_DONE : CLOUD_FAILED);
    return ok;
}

// ---- settings: a profile's folder as one ustar archive -------------------------------------------------------------------------------
typedef struct {
    char *data;
    size_t size, capacity;
} Buffer;
static bool grow(Buffer *b, size_t more) {
    if (b->size + more <= b->capacity) return true;
    size_t capacity = b->capacity ? b->capacity : 65536;
    while (capacity < b->size + more) capacity *= 2;
    if (capacity > (32u << 20)) return false;
    char *bigger = realloc(b->data, capacity);
    if (!bigger) return false;
    b->data = bigger;
    b->capacity = capacity;
    return true;
}
static bool tarEntry(Buffer *b, const char *name, bool folder, const void *data, size_t size) {
    if (strlen(name) > 99 || !grow(b, 512 + ((size + 511) & ~(size_t)511))) return false;
    char *h = b->data + b->size;
    memset(h, 0, 512);
    snprintf(h, 100, "%s", name);
    snprintf(h + 100, 8, "%07o", folder ? 0755 : 0644);
    snprintf(h + 108, 8, "%07o", 0);
    snprintf(h + 116, 8, "%07o", 0);
    snprintf(h + 124, 12, "%011zo", folder ? (size_t)0 : size);
    snprintf(h + 136, 12, "%011o", 0);
    h[156] = folder ? '5' : '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i) sum += (unsigned char)h[i];
    snprintf(h + 148, 8, "%06o", sum);
    b->size += 512;
    if (!folder && size) {
        memcpy(b->data + b->size, data, size);
        memset(b->data + b->size + size, 0, ((size + 511) & ~(size_t)511) - size);
        b->size += (size + 511) & ~(size_t)511;
    }
    return true;
}
static bool tarFolder(Buffer *b, const char *root, const char *relative, unsigned depth) {
    char path[700];
    snprintf(path, sizeof(path), "%s%s%s", root, relative[0] ? "/" : "", relative);
    int error = 0;
    PlatformDirectory *directory = depth < 8 ? platformDirectoryOpen(path, &error) : NULL;
    if (!directory) return depth == 0 ? false : true;
    char name[256];
    uint8_t type;
    uint64_t inode;
    bool ok = true;
    while (ok && platformDirectoryRead(directory, name, &type, &inode, &error) == 1) {
        if (!strcmp(name, ".") || !strcmp(name, "..") || strstr(name, ".part")) continue;
        char child[300], full[800];
        snprintf(child, sizeof(child), "%s%s%s", relative, relative[0] ? "/" : "", name);
        snprintf(full, sizeof(full), "%s/%s", root, child);
        if (type == 4) {
            ok = tarEntry(b, child, true, NULL, 0) && tarFolder(b, root, child, depth + 1);
            continue;
        }
        int fd = open(full, O_RDONLY);
        struct stat info;
        if (fd < 0 || fstat(fd, &info) || info.st_size > (8 << 20)) {
            if (fd >= 0) close(fd);
            continue;
        }
        char *data = malloc((size_t)info.st_size + 1);
        ok = data && read(fd, data, (size_t)info.st_size) == info.st_size && tarEntry(b, child, false, data, (size_t)info.st_size);
        free(data);
        close(fd);
    }
    platformDirectoryClose(directory);
    return ok;
}
static bool untar(const char *data, size_t size, const char *folder) {
    makeFolders(folder);
    for (size_t at = 0; at + 512 <= size;) {
        const char *h = data + at;
        if (!h[0]) break;
        char name[101];
        snprintf(name, sizeof(name), "%.100s", h);
        size_t length = (size_t)strtoull(h + 124, NULL, 8);
        at += 512;
        if (strstr(name, "..") || name[0] == '/' || at + length > size) return false;
        char path[800];
        snprintf(path, sizeof(path), "%s/%s", folder, name);
        if (h[156] == '5')
            makeFolders(path);
        else if (h[156] == '0' || h[156] == 0) {
            char *slash = strrchr(path, '/');
            if (slash) {
                *slash = 0;
                makeFolders(path);
                *slash = '/';
            }
            if (!writeFile(path, data + at, length)) return false;
        }
        at += (length + 511) & ~(size_t)511;
    }
    return true;
}
static uint64_t fnv(const char *data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ (unsigned char)data[i]) * 1099511628211ull;
    return hash;
}
bool cloudBackupProfile(const char *profile, const char *config_folder) {
    Buffer b = {0};
    if (!tarFolder(&b, config_folder, "", 0) || !grow(&b, 1024)) {
        free(b.data);
        return true;  // nothing to back up (yet)
    }
    memset(b.data + b.size, 0, 1024);
    b.size += 1024;
    char sum_path[400], sum[32], last[32] = "";
    snprintf(sum_path, sizeof(sum_path), "%s/profile-%s.sum", state, profile);
    snprintf(sum, sizeof(sum), "%016llx", (unsigned long long)fnv(b.data, b.size));
    if (readFile(sum_path, last, sizeof(last)) && !strcmp(last, sum)) {
        free(b.data);
        return true;
    }
    atomic_store(&status.phase, CLOUD_BUSY);
    setActivity("Backing up settings", b.size);
    char name[128], id[128];
    snprintf(name, sizeof(name), "profile-%s.tar", profile);
    RemoteList old = {0};
    bool ok = folders() && listFolder(top_id, name, &old) && uploadSmall(top_id, name, b.data, b.size, id, sizeof(id));
    if (ok) {  // the new copy is in: the older ones go
        for (unsigned i = 0; i < old.count; ++i)
            if (strcmp(old.files[i].id, id)) deleteFile(old.files[i].id);
        char line[40];
        int length = snprintf(line, sizeof(line), "%s\n", sum);
        writeFile(sum_path, line, (size_t)length);
    }
    free(old.files);
    free(b.data);
    diagnosticsTrace("cloud: settings of profile %s %s", profile, ok ? "backed up" : "NOT backed up");
    atomic_store(&status.phase, ok ? CLOUD_DONE : CLOUD_FAILED);
    return ok;
}
static bool folderEmpty(const char *path) {
    int error = 0;
    PlatformDirectory *directory = platformDirectoryOpen(path, &error);
    if (!directory) return true;
    char name[256];
    uint8_t type;
    uint64_t inode;
    bool empty = true;
    while (empty && platformDirectoryRead(directory, name, &type, &inode, &error) == 1) empty = !strcmp(name, ".") || !strcmp(name, "..");
    platformDirectoryClose(directory);
    return empty;
}
bool cloudRestoreProfiles(const char *users_folder, unsigned *restored) {
    *restored = 0;
    atomic_store(&status.phase, CLOUD_BUSY);
    setActivity("Restoring settings", 0);
    RemoteList list;
    if (!folders() || !listFolder(top_id, NULL, &list)) {
        atomic_store(&status.phase, CLOUD_FAILED);
        return false;
    }
    bool ok = true;
    for (unsigned i = 0; i < list.count; ++i) {
        const RemoteFile *f = &list.files[i];
        char profile[64];
        if (f->folder || sscanf(f->name, "profile-%63[0-9a-z].tar", profile) != 1) continue;
        char config[400], archive[460];
        snprintf(config, sizeof(config), "%s/%s/config", users_folder, profile);
        if (!folderEmpty(config)) continue;  // this console already has settings for that profile: they win
        snprintf(archive, sizeof(archive), "%s/restore-%s.tar", state, profile);
        bool good = download(f->id, archive, f->size);
        if (good) {
            int fd = open(archive, O_RDONLY);
            struct stat info;
            char *data = NULL;
            if (fd >= 0 && !fstat(fd, &info) && (data = malloc((size_t)info.st_size + 1)) && read(fd, data, (size_t)info.st_size) == info.st_size)
                good = untar(data, (size_t)info.st_size, config);
            else
                good = false;
            free(data);
            if (fd >= 0) close(fd);
            char sum_path[400];  // the next backup of this profile compares against nothing: it sends what the game wrote since
            snprintf(sum_path, sizeof(sum_path), "%s/profile-%s.sum", state, profile);
            unlink(sum_path);
        }
        unlink(archive);
        *restored += good;
        ok = ok && good;
        diagnosticsTrace("cloud: settings of profile %s %s", profile, good ? "restored" : "NOT restored");
    }
    free(list.files);
    atomic_store(&status.phase, ok ? CLOUD_DONE : CLOUD_FAILED);
    return ok;
}
