// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// The client updater (see updater.h). The zip is read in place on PokeMMO's server:
//   1. HEAD: its ETag and size (an unchanged ETag means nothing more to do);
//   2. its last 64 KiB: the end-of-central-directory record, then the central directory: the list of entries;
//   3. revision.txt alone, to know which revision it is;
//   4. to update: the selected entries, fetched as a couple of large ranges (entries close together share one request), each
//      unpacked with zlib as it arrives and checked against its CRC-32. A dropped connection resumes from the entry it was in.
// The selection is the installer's (installer/pokemmo_prospero_install.py, client_entries): everything but other systems'
// binaries, launchers, logs and ROMs.
#include "updater.h"
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
#include <zlib.h>

#define RUN_GAP (4u << 20)    // entries closer than this are fetched in one request
#define ENTRY_SLACK 1024u     // a local header's extra field may differ from the central directory's
#define MAX_ATTEMPTS 6  // per entry; a request that makes progress does not count against it


static void fail(char *error, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void fail(char *error, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
    diagnosticsTrace("updater: %s", error);
}
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int updaterCompareRevisions(const char *a, const char *b) {
    long x = a && *a ? strtol(a, NULL, 10) : -1, y = b && *b ? strtol(b, NULL, 10) : -1;
    return x < y ? -1 : x > y;
}

// ---- reading a range as a stream ----------------------------------------------------------------------------------------------------
typedef struct {
    PlatformHttp *http;
    uint64_t position;  // zip offset of the next byte
} Stream;
static bool streamOpen(Stream *s, const char *url, uint64_t start, uint64_t end, const char *etag, char *error, size_t size) {
    int status = 0;
    char reason[160] = "";
    s->http = platformHttpOpen(url, false, (int64_t)start, (int64_t)end, &status, reason, sizeof(reason));
    if (!s->http) {
        fail(error, size, "download failed: %s", reason);
        return false;
    }
    char value[128];
    if (etag && etag[0] && platformHttpHeader(s->http, "ETag", value, sizeof(value)) && strcmp(value, etag)) {
        fail(error, size, "the client was replaced on the server while downloading (try again)");
        platformHttpClose(s->http);
        return false;
    }
    if (status == 206)
        s->position = start;
    else if (status == 200)
        s->position = 0;  // the whole file: what comes before `start` is skipped
    else {
        fail(error, size, "the server answered %d", status);
        platformHttpClose(s->http);
        return false;
    }
    return true;
}
static void streamClose(Stream *s) {
    platformHttpClose(s->http);
    s->http = NULL;
}
static bool streamRead(Stream *s, void *buffer, size_t size) {
    unsigned char *out = buffer;
    while (size) {
        int64_t got = platformHttpRead(s->http, out, size);
        if (got <= 0) return false;
        out += got;
        size -= (size_t)got;
        s->position += (uint64_t)got;
    }
    return true;
}
static bool streamSkipTo(Stream *s, uint64_t offset) {
    static unsigned char scratch[65536];
    if (offset < s->position) return false;
    while (s->position < offset) {
        uint64_t left = offset - s->position;
        if (!streamRead(s, scratch, left < sizeof(scratch) ? (size_t)left : sizeof(scratch))) return false;
    }
    return true;
}

// ---- the central directory ----------------------------------------------------------------------------------------------------------
static bool wanted(const char *name) {
    size_t length = strlen(name);
    if (!length || name[length - 1] == '/' || name[0] == '/' || strstr(name, "..")) return false;
    if (!strncmp(name, "bin/", 4) && strncmp(name, "bin/linux/x64/", 14)) return false;  // Windows, macOS and ARM binaries
    static const char *const launchers[] = {".exe", ".sh", ".bat", ".command"};
    for (unsigned i = 0; i < 4; ++i) {
        size_t n = strlen(launchers[i]);
        if (length >= n && !strcasecmp(name + length - n, launchers[i])) return false;
    }
    return strncmp(name, "log/", 4) && strncmp(name, "roms/", 5);
}
static unsigned char *fetch(const char *url, uint64_t start, uint64_t end, const char *etag, char *error, size_t size) {
    Stream s;
    if (!streamOpen(&s, url, start, end, etag, error, size)) return NULL;
    unsigned char *data = malloc((size_t)(end - start + 1));
    bool ok = data && streamSkipTo(&s, start) && streamRead(&s, data, (size_t)(end - start + 1));
    streamClose(&s);
    if (!ok) {
        free(data);
        fail(error, size, "the connection dropped while reading the client's index");
        return NULL;
    }
    return data;
}
static bool parseDirectory(const unsigned char *cd, uint32_t cd_size, unsigned total, UpdaterRemote *r, char *error, size_t size) {
    r->entries = calloc(total ? total : 1, sizeof(UpdaterEntry));
    if (!r->entries) return false;
    const unsigned char *p = cd, *end = cd + cd_size;
    for (unsigned i = 0; i < total; ++i) {
        if (p + 46 > end || le32(p) != 0x02014b50u) {
            fail(error, size, "the client's index is damaged (entry %u)", i);
            return false;
        }
        uint16_t name_length = le16(p + 28), extra_length = le16(p + 30), comment_length = le16(p + 32);
        if (p + 46 + name_length > end) {
            fail(error, size, "the client's index is damaged (name %u)", i);
            return false;
        }
        char name[512];
        snprintf(name, sizeof(name), "%.*s", (int)name_length, (const char *)p + 46);
        uint32_t compressed = le32(p + 20), uncompressed = le32(p + 24), offset = le32(p + 42);
        if (compressed == 0xFFFFFFFFu || uncompressed == 0xFFFFFFFFu || offset == 0xFFFFFFFFu) {
            fail(error, size, "the client zip needs ZIP64, which is not supported (%s)", name);
            return false;
        }
        uint16_t method = le16(p + 10);
        if (wanted(name)) {
            if (method != 0 && method != 8) {
                fail(error, size, "%s uses compression method %u", name, method);
                return false;
            }
            UpdaterEntry *e = &r->entries[r->count++];
            e->name = strdup(name);
            e->crc = le32(p + 16);
            e->compressed = compressed;
            e->size = uncompressed;
            e->offset = offset;
            e->method = method;
            e->name_length = name_length;
            e->extra_length = extra_length;
            e->executable = ((le32(p + 38) >> 16) & 0111) != 0;
        }
        p += 46 + name_length + extra_length + comment_length;
    }
    // In zip order, so that each range is read front to back.
    for (unsigned i = 1; i < r->count; ++i)
        for (unsigned j = i; j > 0 && r->entries[j - 1].offset > r->entries[j].offset; --j) {
            UpdaterEntry t = r->entries[j];
            r->entries[j] = r->entries[j - 1];
            r->entries[j - 1] = t;
        }
    return true;
}
static uint64_t entryEnd(const UpdaterRemote *r, const UpdaterEntry *e) {  // last byte that may belong to it
    uint64_t end = (uint64_t)e->offset + 30u + e->name_length + e->extra_length + e->compressed + ENTRY_SLACK;
    return end < (uint64_t)r->length ? end : (uint64_t)r->length - 1;
}
// Entries [first, last] that share one request.
static unsigned runEnd(const UpdaterRemote *r, unsigned first) {
    unsigned last = first;
    while (last + 1 < r->count && r->entries[last + 1].offset <= entryEnd(r, &r->entries[last]) + RUN_GAP) ++last;
    return last;
}

// ---- one entry -------------------------------------------------------------------------------------------------------------------------
enum { ENTRY_OK = 0, ENTRY_NETWORK = -1, ENTRY_DATA = -2, ENTRY_DISK = -3 };
static bool makeParents(const char *path) {
    char partial[1024];
    snprintf(partial, sizeof(partial), "%s", path);
    for (char *slash = strchr(partial + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        if (mkdir(partial, 0755) && errno != EEXIST) return false;
        *slash = '/';
    }
    return true;
}
// One entry being unpacked. It survives a dropped connection: the next request starts where the data stopped, and zlib carries
// on as if nothing happened.
typedef struct {
    const UpdaterEntry *e;
    bool header_done;
    uint64_t data_start;  // zip offset of the entry's data
    uint32_t left, crc, produced;
    z_stream z;
    int z_status;
} Unpack;
static void unpackBegin(Unpack *u, const UpdaterEntry *e) {
    memset(u, 0, sizeof(*u));
    u->e = e;
    u->left = e->compressed;
    u->crc = (uint32_t)crc32(0, NULL, 0);
    u->z_status = Z_OK;
    if (e->method == 8) inflateInit2(&u->z, -MAX_WBITS);
}
static void unpackEnd(Unpack *u) {
    if (u->e->method == 8) inflateEnd(&u->z);
}
static uint64_t unpackPosition(const Unpack *u) {  // where a new request has to start
    return u->header_done ? u->data_start + (u->e->compressed - u->left) : u->e->offset;
}
// Carries the entry on from the stream (positioned at or before what it needs next) into `out` (a file) or `memory`.
static int unpackStep(Stream *s, Unpack *u, int out, unsigned char *memory, size_t memory_size, UpdaterProgress *progress, char *error, size_t size) {
    const UpdaterEntry *e = u->e;
    if (!u->header_done) {
        unsigned char header[30];
        if (!streamSkipTo(s, e->offset) || !streamRead(s, header, sizeof(header))) return ENTRY_NETWORK;
        if (le32(header) != 0x04034b50u) {
            fail(error, size, "%s: no local header where the index says", e->name);
            return ENTRY_DATA;
        }
        u->data_start = (uint64_t)e->offset + 30u + le16(header + 26) + le16(header + 28);
        u->header_done = true;
    }
    if (!streamSkipTo(s, unpackPosition(u))) return ENTRY_NETWORK;
    static unsigned char input[65536], output[262144];
    for (;;) {
        bool finished = e->method == 8 ? u->z_status == Z_STREAM_END : !u->left;
        if (finished) break;
        size_t chunk = u->left < sizeof(input) ? u->left : sizeof(input);
        if (!chunk) {
            fail(error, size, "%s: the data ends early", e->name);
            return ENTRY_DATA;
        }
        if (!streamRead(s, input, chunk)) return ENTRY_NETWORK;  // a short read is lost: the request is made again from the same place
        u->left -= (uint32_t)chunk;
        if (progress) atomic_fetch_add(&progress->done, chunk);
        u->z.next_in = input;
        u->z.avail_in = (uInt)chunk;
        do {
            const unsigned char *data = input;
            size_t data_size = chunk;
            if (e->method == 8) {
                u->z.next_out = output;
                u->z.avail_out = sizeof(output);
                u->z_status = inflate(&u->z, Z_NO_FLUSH);
                if (u->z_status != Z_OK && u->z_status != Z_STREAM_END) {
                    fail(error, size, "%s: damaged data (zlib %d)", e->name, u->z_status);
                    return ENTRY_DATA;
                }
                data = output;
                data_size = sizeof(output) - u->z.avail_out;
            }
            if (data_size) {
                u->crc = (uint32_t)crc32(u->crc, data, (uInt)data_size);
                if (memory) {
                    if (u->produced + data_size > memory_size) return ENTRY_DATA;
                    memcpy(memory + u->produced, data, data_size);
                } else
                    for (size_t done = 0; done < data_size;) {
                        ssize_t wrote = write(out, data + done, data_size - done);
                        if (wrote <= 0) {
                            fail(error, size, "%s: writing failed (errno %d; storage full?)", e->name, errno);
                            return ENTRY_DISK;
                        }
                        done += (size_t)wrote;
                    }
                u->produced += (uint32_t)data_size;
            }
        } while (e->method == 8 && u->z_status != Z_STREAM_END && (u->z.avail_in || u->z.avail_out == 0));
    }
    if (u->crc != e->crc || u->produced != e->size) {
        fail(error, size, "%s: check failed (crc %08x, expected %08x; %u of %u bytes)", e->name, u->crc, e->crc, u->produced, e->size);
        return ENTRY_DATA;
    }
    return ENTRY_OK;
}

// ---- check ------------------------------------------------------------------------------------------------------------------------------
int updaterCheck(const char *url, const char *known_etag, UpdaterRemote *r, char *error, size_t size) {
    memset(r, 0, sizeof(*r));
    int status = 0;
    char reason[160] = "", value[128];
    PlatformHttp *head = platformHttpOpen(url, true, 0, -1, &status, reason, sizeof(reason));
    if (!head) {
        fail(error, size, "cannot reach the PokeMMO download server: %s", reason);
        return -1;
    }
    if (status != 200) {
        platformHttpClose(head);
        fail(error, size, "the PokeMMO download server answered %d", status);
        return -1;
    }
    if (platformHttpHeader(head, "ETag", value, sizeof(value))) snprintf(r->etag, sizeof(r->etag), "%s", value);
    r->length = platformHttpLength(head);
    if (r->length < 0 && platformHttpHeader(head, "Content-Length", value, sizeof(value))) r->length = atoll(value);
    platformHttpClose(head);
    diagnosticsTrace("updater: published zip etag=%s length=%lld", r->etag, (long long)r->length);
    if (known_etag && known_etag[0] && r->etag[0] && !strcmp(known_etag, r->etag)) return 1;
    if (r->length < 22) {
        fail(error, size, "the download server did not say how large the client is");
        return -1;
    }
    uint64_t tail_size = r->length < 65557 ? (uint64_t)r->length : 65557u, tail_start = (uint64_t)r->length - tail_size;
    unsigned char *tail = fetch(url, tail_start, (uint64_t)r->length - 1, r->etag, error, size);
    if (!tail) return -1;
    const unsigned char *eocd = NULL;
    for (uint64_t i = tail_size - 22 + 1; i-- > 0;)
        if (le32(tail + i) == 0x06054b50u) {
            eocd = tail + i;
            break;
        }
    if (!eocd) {
        free(tail);
        fail(error, size, "the download is not a zip file");
        return -1;
    }
    unsigned total = le16(eocd + 10);
    uint32_t cd_size = le32(eocd + 12), cd_offset = le32(eocd + 16);
    if ((uint64_t)cd_offset + cd_size > (uint64_t)r->length) {
        free(tail);
        fail(error, size, "the client's index is outside the file");
        return -1;
    }
    unsigned char *cd = NULL;
    if (cd_offset >= tail_start)
        cd = tail + (cd_offset - tail_start);
    else if (!(cd = fetch(url, cd_offset, (uint64_t)cd_offset + cd_size - 1, r->etag, error, size))) {
        free(tail);
        return -1;
    }
    bool parsed = parseDirectory(cd, cd_size, total, r, error, size);
    if (cd < tail || cd >= tail + tail_size) free(cd);
    free(tail);
    if (!parsed) return -1;
    const UpdaterEntry *revision = NULL, *binary = NULL;
    for (unsigned i = 0; i < r->count; ++i) {
        if (!strcmp(r->entries[i].name, "revision.txt")) revision = &r->entries[i];
        if (!strcmp(r->entries[i].name, "bin/linux/x64/PokeMMO")) binary = &r->entries[i];
    }
    if (!revision || !binary || revision->size >= sizeof(r->revision)) {
        fail(error, size, "the download has no Linux client or revision.txt");
        return -1;
    }
    Stream s;
    if (!streamOpen(&s, url, revision->offset, entryEnd(r, revision), r->etag, error, size)) return -1;
    unsigned char text[32] = {0};
    Unpack u;
    unpackBegin(&u, revision);
    int unpacked = unpackStep(&s, &u, -1, text, sizeof(text) - 1, NULL, error, size);
    unpackEnd(&u);
    streamClose(&s);
    if (unpacked != ENTRY_OK) {
        if (unpacked == ENTRY_NETWORK) fail(error, size, "the connection dropped while reading revision.txt");
        return -1;
    }
    snprintf(r->revision, sizeof(r->revision), "%.*s", (int)strcspn((char *)text, "\r\n \t"), (char *)text);
    for (unsigned first = 0; first < r->count;) {
        unsigned last = runEnd(r, first);
        r->download_bytes += entryEnd(r, &r->entries[last]) - r->entries[first].offset + 1;
        first = last + 1;
    }
    diagnosticsTrace("updater: published revision %s, %u files for the PS5, %llu bytes to download", r->revision, r->count,
                     (unsigned long long)r->download_bytes);
    return 0;
}

// ---- download -------------------------------------------------------------------------------------------------------------------------
int updaterDownload(const char *url, const UpdaterRemote *r, const char *staging, UpdaterProgress *progress, char *error, size_t size) {
    atomic_store(&progress->done, 0);
    atomic_store(&progress->total, r->download_bytes);
    Stream s = {0};
    for (unsigned first = 0; first < r->count;) {
        unsigned last = runEnd(r, first);
        uint64_t run_end = entryEnd(r, &r->entries[last]);
        for (unsigned index = first; index <= last; ++index) {
            const UpdaterEntry *e = &r->entries[index];
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", staging, e->name);
            int out = makeParents(path) ? open(path, O_WRONLY | O_CREAT | O_TRUNC, e->executable ? 0755 : 0644) : -1;
            if (out < 0) {
                fail(error, size, "cannot write %s (errno %d)", path, errno);
                if (s.http) streamClose(&s);
                return -1;
            }
            uint64_t progress_before = atomic_load(&progress->done);
            Unpack u;
            unpackBegin(&u, e);
            int result = ENTRY_NETWORK;
            for (unsigned attempts = 0; result != ENTRY_OK;) {
                uint64_t position_before = unpackPosition(&u);
                if (!s.http && !streamOpen(&s, url, unpackPosition(&u), run_end, r->etag, error, size)) {
                    result = ENTRY_NETWORK;
                } else {
                    result = unpackStep(&s, &u, out, NULL, 0, progress, error, size);
                    if (result == ENTRY_OK) break;
                    streamClose(&s);
                }
                if (result == ENTRY_NETWORK && unpackPosition(&u) > position_before) attempts = 0;  // it got further: not a failure
                if (result == ENTRY_DISK || ++attempts >= MAX_ATTEMPTS) break;
                diagnosticsTrace("updater: %s in %s, attempt %u", result == ENTRY_NETWORK ? "connection lost" : "bad data", e->name, attempts);
                if (result == ENTRY_DATA) {  // from the start of the entry again, with a new request
                    unpackEnd(&u);
                    unpackBegin(&u, e);
                    atomic_store(&progress->done, progress_before);
                    if (ftruncate(out, 0) || lseek(out, 0, SEEK_SET)) result = ENTRY_DISK;
                }
                platformSleepNs(1000000000ull);
            }
            unpackEnd(&u);
            close(out);
            if (result != ENTRY_OK) {
                if (s.http) streamClose(&s);
                if (result == ENTRY_NETWORK) fail(error, size, "the connection kept dropping while downloading");
                return -1;
            }
        }
        if (s.http) streamClose(&s);
        first = last + 1;
    }
    atomic_store(&progress->done, r->download_bytes);
    return 0;
}

// ---- apply ------------------------------------------------------------------------------------------------------------------------------
int updaterApply(const UpdaterRemote *r, const char *staging, const char *game, char *error, size_t size) {
    const UpdaterEntry *revision = NULL;
    unsigned moved = 0, kept = 0;
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < r->count; ++i) {
            const UpdaterEntry *e = &r->entries[i];
            bool is_revision = !strcmp(e->name, "revision.txt");
            if (is_revision) revision = e;
            if (is_revision != (pass == 1)) continue;  // revision.txt last
            char from[1024], to[1024];
            snprintf(from, sizeof(from), "%s/%s", staging, e->name);
            snprintf(to, sizeof(to), "%s/%s", game, e->name);
            struct stat info;
            if (!strncmp(e->name, "config/", 7) && !stat(to, &info)) {  // the player's settings stay
                unlink(from);
                ++kept;
                continue;
            }
            if (!makeParents(to) || rename(from, to)) {
                fail(error, size, "cannot move %s into place (errno %d)", e->name, errno);
                return -1;
            }
            ++moved;
        }
    diagnosticsTrace("updater: applied revision %s: %u files moved, %u settings files kept", r->revision, moved, kept);
    return revision ? 0 : -1;
}

void updaterFree(UpdaterRemote *r) {
    for (unsigned i = 0; i < r->count; ++i) free(r->entries[i].name);
    free(r->entries);
    memset(r, 0, sizeof(*r));
}
