// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 PokeMMO-Prospero contributors
//
// ROM upload servers (see upload_server.h). Plain BSD sockets, the same on the PS5 and a PC. Each connection gets a thread;
// sockets wait with poll() so that stopping never hangs on a blocked accept. Everything written lands in the ROM folder:
// a name with a slash, "..", or a control character is refused, and every file arrives under a temporary name first.
//
// FTP: enough of RFC 959/2428/3659 for FTP apps and Python's ftplib (the installer): USER/PASS (anything), PWD, CWD,
// CDUP, TYPE, PASV, EPSV, LIST, NLST, MLSD, STOR, SIZE, DELE, RNFR/RNTO, MKD, NOOP, QUIT. The paths above the ROM folder
// (/data, /data/homebrew, ...) exist only so that clients can walk down to it.
#include "upload_server.h"
#include "diagnostics.h"
#include "platform.h"
#include "roms.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BANNER "220 PokeMMO-Prospero ROM upload"
static UploadStatus status;
static char folder[256], ftp_path[256];
static void (*on_change)(void);
static _Atomic bool stopping;
static int listeners[2] = {-1, -1};
static pthread_t acceptors[2];
static _Atomic int connections;

UploadStatus *uploadStatus(void) { return &status; }

// ---- small socket helpers ------------------------------------------------------------------------------------------------------
static bool sendAll(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t sent = send(fd, p, size, 0);
        if (sent <= 0) return false;
        p += sent;
        size -= (size_t)sent;
    }
    return true;
}
static bool sendText(int fd, const char *text) { return sendAll(fd, text, strlen(text)); }
static void timeouts(int fd, unsigned seconds) {
    struct timeval tv = {(time_t)seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
static int listenOn(unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 4)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int acceptWithin(int listener, unsigned milliseconds) {
    struct pollfd entry = {.fd = listener, .events = POLLIN};
    for (unsigned waited = 0; waited < milliseconds && !atomic_load(&stopping); waited += 250)
        if (poll(&entry, 1, 250) > 0) return accept(listener, NULL, NULL);
    return -1;
}

// ---- names and files ------------------------------------------------------------------------------------------------------------
static bool safeName(const char *name) {
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) > 200) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == '/' || *p == '\\') return false;
    return true;
}
// Receives into the ROM folder: `length` bytes, or until the other side closes when length < 0. `prefix` (`size`) is data
// already read. Arrives as ".<name>.receiving", renamed when complete.
static bool receiveFile(int fd, const char *name, int64_t length, const char *prefix, size_t prefix_size) {
    char temporary[512], final_path[512];
    snprintf(temporary, sizeof(temporary), "%s/.%s.receiving", folder, name);
    snprintf(final_path, sizeof(final_path), "%s/%s", folder, name);
    int out = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) return false;
    snprintf(status.current, sizeof(status.current), "%s", name);
    atomic_store(&status.current_total, length > 0 ? (uint64_t)length : 0);
    atomic_store(&status.current_done, 0);
    static _Thread_local char buffer[262144];
    uint64_t done = 0;
    bool ok = true;
    if (prefix_size) {
        size_t take = length >= 0 && (int64_t)prefix_size > length ? (size_t)length : prefix_size;
        ok = write(out, prefix, take) == (ssize_t)take;
        done += take;
    }
    while (ok && (length < 0 || done < (uint64_t)length)) {
        size_t want = sizeof(buffer);
        if (length >= 0 && (uint64_t)length - done < want) want = (size_t)((uint64_t)length - done);
        ssize_t got = recv(fd, buffer, want, 0);
        if (got <= 0) {
            ok = length < 0 && got == 0;  // FTP: the end of the data connection is the end of the file
            break;
        }
        for (ssize_t written = 0; written < got && ok;) {
            ssize_t n = write(out, buffer + written, (size_t)(got - written));
            if (n <= 0) ok = false;
            written += n;
        }
        done += (uint64_t)got;
        atomic_store(&status.current_done, done);
    }
    close(out);
    status.current[0] = 0;
    if (!ok || rename(temporary, final_path)) {
        unlink(temporary);
        diagnosticsTrace("upload: %s FAILED after %llu bytes", name, (unsigned long long)done);
        return false;
    }
    atomic_fetch_add(&status.received, 1);
    diagnosticsTrace("upload: %s received, %llu bytes", name, (unsigned long long)done);
    if (on_change) on_change();
    return true;
}

// ---- the web page --------------------------------------------------------------------------------------------------------------
static void urlDecode(char *text) {
    char *out = text;
    for (char *in = text; *in; ++in) {
        unsigned value;
        if (*in == '%' && in[1] && in[2] && sscanf(in + 1, "%2x", &value) == 1) {
            *out++ = (char)value;
            in += 2;
        } else
            *out++ = *in == '+' ? ' ' : *in;
    }
    *out = 0;
}
static void htmlEscape(char *out, size_t size, const char *text) {
    size_t used = 0;
    for (; *text && used + 7 < size; ++text) {
        const char *entity = *text == '<' ? "&lt;" : *text == '>' ? "&gt;" : *text == '&' ? "&amp;" : *text == '"' ? "&quot;" : NULL;
        if (entity) {
            memcpy(out + used, entity, strlen(entity));
            used += strlen(entity);
        } else
            out[used++] = *text;
    }
    out[used] = 0;
}
static void sendPage(int fd) {
    static _Thread_local RomScan scan;
    romsScan(folder, &scan);
    size_t capacity = 32768;
    char *page = malloc(capacity), escaped[600];
    if (!page) return;
    int n = snprintf(page, capacity,
                     "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
                     "<title>PokeMMO Prospero ROMs</title><style>body{font-family:system-ui,sans-serif;background:#081228;color:#e8eefc;margin:0;padding:24px}"
                     "main{max-width:760px;margin:auto}h1{font-weight:500}table{width:100%%;border-collapse:collapse;margin:16px 0}"
                     "td{padding:10px;border-bottom:1px solid #1b2b4f}.ok{color:#7ee08a}.miss{color:#f0c060}.req{color:#e05555}"
                     ".box{background:#111d36;border-radius:12px;padding:20px;margin-top:20px}button,label.btn{background:#5aa0e8;color:#081228;"
                     "border:0;border-radius:8px;padding:12px 20px;font-size:16px;cursor:pointer;display:inline-block}"
                     "progress{width:100%%;height:14px;margin-top:12px}#log{margin-top:10px;color:#a8b8d8;white-space:pre-line}</style></head><body><main>"
                     "<h1>PokeMMO Prospero: game ROMs</h1><p>PokeMMO needs <b>Pokemon Black or White</b>. FireRed, Emerald, Platinum and "
                     "HeartGold/SoulSilver add more regions.</p><table>");
    for (int game = 0; game < ROM_GAMES; ++game) {
        int index = scan.found[game];
        if (index >= 0) htmlEscape(escaped, sizeof(escaped), scan.files[index].note);
        n += snprintf(page + n, capacity - (size_t)n, "<tr><td>%s</td><td>%s</td><td class=%s>%s</td></tr>", romGameName(game),
                      romGameRequired(game) ? "Required" : "Optional", index >= 0 ? "ok" : romGameRequired(game) ? "req" : "miss",
                      index >= 0 ? escaped : "Missing");
    }
    n += snprintf(page + n, capacity - (size_t)n, "</table>");
    for (unsigned i = 0; i < scan.count && (size_t)n < capacity - 1200; ++i)
        if (scan.files[i].game < 0) {
            char name[300];
            htmlEscape(name, sizeof(name), scan.files[i].file);
            htmlEscape(escaped, sizeof(escaped), scan.files[i].note);
            n += snprintf(page + n, capacity - (size_t)n, "<p class=miss>%s: %s</p>", name, escaped);
        }
    n += snprintf(page + n, capacity - (size_t)n,
                  "<div class=box><label class=btn>Choose ROM files<input id=f type=file multiple accept='.nds,.gba' hidden></label>"
                  "<progress id=p value=0 max=1 hidden></progress><div id=log></div></div>"
                  "<p style='color:#8a9bbf'>The files go to the PS5 title's roms folder. When you are done, press Cross on the console.</p>"
                  "<script>const f=document.getElementById('f'),p=document.getElementById('p'),log=document.getElementById('log');"
                  "f.onchange=async()=>{for(const file of f.files){p.hidden=false;log.textContent='Sending '+file.name+'...';"
                  "await new Promise((ok,bad)=>{const x=new XMLHttpRequest();x.open('PUT','/upload/'+encodeURIComponent(file.name));"
                  "x.upload.onprogress=e=>{p.value=e.loaded/e.total};x.onload=()=>x.status==200?ok():bad(x.responseText);x.onerror=()=>bad('connection lost');"
                  "x.send(file)}).then(()=>{log.textContent=file.name+' sent.'},e=>{log.textContent=file.name+': '+e});}location.reload()};"
                  "</script></main></body></html>");
    char header[160];
    snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", n);
    if (sendText(fd, header)) sendAll(fd, page, (size_t)n);
    free(page);
}
static void sendStatus(int fd, int code, const char *reason, const char *body) {
    char response[512];
    snprintf(response, sizeof(response), "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", code,
             reason, strlen(body), body);
    sendText(fd, response);
}
static void serveHttp(int fd) {
    char request[8192];
    size_t have = 0;
    char *end = NULL;
    while (!end && have < sizeof(request) - 1) {
        ssize_t got = recv(fd, request + have, sizeof(request) - 1 - have, 0);
        if (got <= 0) return;
        have += (size_t)got;
        request[have] = 0;
        end = strstr(request, "\r\n\r\n");
    }
    if (!end) { sendStatus(fd, 400, "Bad Request", "request too large"); return; }
    char method[16], target[1024];
    if (sscanf(request, "%15s %1023s", method, target) != 2) { sendStatus(fd, 400, "Bad Request", "bad request"); return; }
    long long length = -1;
    for (char *line = strstr(request, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n"))
        if (!strncasecmp(line + 2, "Content-Length:", 15)) length = atoll(line + 17);
    if (!strcmp(method, "GET") && (!strcmp(target, "/") || !strncmp(target, "/?", 2))) { sendPage(fd); return; }
    if (!strcmp(method, "PUT") && !strncmp(target, "/upload/", 8)) {
        char name[1024];
        snprintf(name, sizeof(name), "%s", target + 8);
        urlDecode(name);
        if (!safeName(name) || name[0] == '.') { sendStatus(fd, 400, "Bad Request", "that file name is not allowed"); return; }
        if (length < 0) { sendStatus(fd, 411, "Length Required", "no length"); return; }
        timeouts(fd, 60);
        char *body = end + 4;
        bool ok = receiveFile(fd, name, length, body, (size_t)(request + have - body));
        return ok ? sendStatus(fd, 200, "OK", "ok") : sendStatus(fd, 500, "Error", "the file could not be stored (storage full?)");
    }
    sendStatus(fd, 404, "Not Found", "not found");
}

// ---- FTP ---------------------------------------------------------------------------------------------------------------------------
typedef struct {
    int control, passive;
    char cwd[512], rename_from[256];
} Ftp;
static void reply(Ftp *f, const char *text) {
    char line[700];
    snprintf(line, sizeof(line), "%s\r\n", text);
    sendText(f->control, line);
}
// An absolute, normalized form of `argument` against the working directory.
static void resolve(const Ftp *f, const char *argument, char *out, size_t size) {
    char joined[1024];
    if (argument[0] == '/')
        snprintf(joined, sizeof(joined), "%s", argument);
    else
        snprintf(joined, sizeof(joined), "%s/%s", f->cwd, argument);
    char result[1024] = "";
    for (char *part = strtok(joined, "/"); part; part = strtok(NULL, "/")) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            char *slash = strrchr(result, '/');
            if (slash) *slash = 0;
            continue;
        }
        strncat(result, "/", sizeof(result) - strlen(result) - 1);
        strncat(result, part, sizeof(result) - strlen(result) - 1);
    }
    snprintf(out, size, "%s", result[0] ? result : "/");
}
// A directory that exists for FTP: the ROM folder and the folders above it.
static bool isDirectory(const char *path) {
    size_t length = strlen(path);
    return !strcmp(path, "/") || !strcmp(path, ftp_path) || (!strncmp(ftp_path, path, length) && ftp_path[length] == '/');
}
// The file name a path stands for in the ROM folder, or NULL.
static const char *fileName(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t directory = (size_t)(slash - path);
    if (!slash || directory != strlen(ftp_path) || strncmp(path, ftp_path, directory)) return NULL;
    return safeName(slash + 1) ? slash + 1 : NULL;
}
static int openPassive(Ftp *f, bool extended) {
    if (f->passive >= 0) close(f->passive);
    f->passive = listenOn(0);
    struct sockaddr_in local = {0}, data = {0};
    socklen_t length = sizeof(local);
    getsockname(f->control, (struct sockaddr *)&local, &length);
    length = sizeof(data);
    if (f->passive < 0 || getsockname(f->passive, (struct sockaddr *)&data, &length)) {
        reply(f, "425 Cannot open a data connection");
        return -1;
    }
    unsigned port = ntohs(data.sin_port);
    uint32_t ip = ntohl(local.sin_addr.s_addr);
    char text[128];
    if (extended)
        snprintf(text, sizeof(text), "229 Entering Extended Passive Mode (|||%u|)", port);
    else
        snprintf(text, sizeof(text), "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port >> 8, port & 255);
    reply(f, text);
    return 0;
}
static int dataConnection(Ftp *f) {
    if (f->passive < 0) {
        reply(f, "425 Use PASV or EPSV first");
        return -1;
    }
    int fd = acceptWithin(f->passive, 20000);
    close(f->passive);
    f->passive = -1;
    if (fd < 0) reply(f, "425 No data connection");
    else timeouts(fd, 60);
    return fd;
}
static void listing(Ftp *f, const char *argument, int kind /* 0 LIST, 1 NLST, 2 MLSD */) {
    char path[1024];
    resolve(f, argument && argument[0] && argument[0] != '-' ? argument : ".", path, sizeof(path));
    if (!isDirectory(path)) { reply(f, "550 No such folder"); return; }
    reply(f, "150 Here it comes");
    int fd = dataConnection(f);
    if (fd < 0) return;
    char line[700];
    if (strcmp(path, ftp_path)) {  // a folder above the ROM folder: its one child
        const char *child = ftp_path + (strcmp(path, "/") ? strlen(path) : 0) + 1;
        char name[256];
        snprintf(name, sizeof(name), "%.*s", (int)strcspn(child, "/"), child);
        if (kind == 0) snprintf(line, sizeof(line), "drwxr-xr-x 1 ps5 ps5 0 Jan  1 00:00 %s\r\n", name);
        if (kind == 1) snprintf(line, sizeof(line), "%s\r\n", name);
        if (kind == 2) snprintf(line, sizeof(line), "type=dir; %s\r\n", name);
        sendText(fd, line);
    } else {
        int error = 0;
        PlatformDirectory *directory = platformDirectoryOpen(folder, &error);
        char name[256];
        uint8_t type;
        uint64_t inode;
        while (directory && platformDirectoryRead(directory, name, &type, &inode, &error) == 1) {
            if (type == 4) continue;
            char full[600];
            struct stat info;
            snprintf(full, sizeof(full), "%s/%s", folder, name);
            long long size = stat(full, &info) ? 0 : (long long)info.st_size;
            if (kind == 0) snprintf(line, sizeof(line), "-rw-r--r-- 1 ps5 ps5 %lld Jan  1 00:00 %s\r\n", size, name);
            if (kind == 1) snprintf(line, sizeof(line), "%s/%s\r\n", argument && argument[0] == '/' ? path : ".", name);
            if (kind == 1 && !(argument && argument[0])) snprintf(line, sizeof(line), "%s\r\n", name);
            if (kind == 2) snprintf(line, sizeof(line), "type=file;size=%lld; %s\r\n", size, name);
            sendText(fd, line);
        }
        if (directory) platformDirectoryClose(directory);
    }
    close(fd);
    reply(f, "226 Done");
}
static void serveFtp(int control) {
    Ftp f = {.control = control, .passive = -1};
    snprintf(f.cwd, sizeof(f.cwd), "%s", ftp_path);
    timeouts(control, 300);
    reply(&f, BANNER);
    char buffer[1024];
    size_t have = 0;
    for (;;) {
        char *newline;
        while (!(newline = memchr(buffer, '\n', have))) {
            if (have >= sizeof(buffer) - 1) have = 0;
            ssize_t got = recv(control, buffer + have, sizeof(buffer) - 1 - have, 0);
            if (got <= 0 || atomic_load(&stopping)) goto done;
            have += (size_t)got;
        }
        *newline = 0;
        if (newline > buffer && newline[-1] == '\r') newline[-1] = 0;
        char line[1024];
        snprintf(line, sizeof(line), "%s", buffer);
        size_t used = (size_t)(newline + 1 - buffer);
        memmove(buffer, newline + 1, have - used);
        have -= used;
        char command[8] = "";
        sscanf(line, "%7s", command);
        for (char *c = command; *c; ++c)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        const char *argument = line + strlen(command);
        while (*argument == ' ') ++argument;
        char path[1024], text[700];
        if (!strcmp(command, "USER")) reply(&f, "331 Any password will do");
        else if (!strcmp(command, "PASS")) reply(&f, "230 Logged in");
        else if (!strcmp(command, "SYST")) reply(&f, "215 UNIX Type: L8");
        else if (!strcmp(command, "FEAT")) sendText(control, "211-Features:\r\n PASV\r\n EPSV\r\n SIZE\r\n MLSD\r\n UTF8\r\n211 End\r\n");
        else if (!strcmp(command, "OPTS") || !strcmp(command, "TYPE") || !strcmp(command, "MODE") || !strcmp(command, "STRU") || !strcmp(command, "NOOP"))
            reply(&f, "200 OK");
        else if (!strcmp(command, "PWD") || !strcmp(command, "XPWD")) {
            snprintf(text, sizeof(text), "257 \"%s\"", f.cwd);
            reply(&f, text);
        } else if (!strcmp(command, "CWD") || !strcmp(command, "CDUP")) {
            resolve(&f, !strcmp(command, "CDUP") ? ".." : argument, path, sizeof(path));
            if (isDirectory(path)) {
                snprintf(f.cwd, sizeof(f.cwd), "%s", path);
                reply(&f, "250 OK");
            } else
                reply(&f, "550 Only the ROM folder is here");
        } else if (!strcmp(command, "MKD")) {
            resolve(&f, argument, path, sizeof(path));
            reply(&f, isDirectory(path) ? "257 Already there" : "550 Only the ROM folder is here");
        } else if (!strcmp(command, "PASV")) openPassive(&f, false);
        else if (!strcmp(command, "EPSV")) openPassive(&f, true);
        else if (!strcmp(command, "LIST")) listing(&f, argument, 0);
        else if (!strcmp(command, "NLST")) listing(&f, argument, 1);
        else if (!strcmp(command, "MLSD")) listing(&f, argument, 2);
        else if (!strcmp(command, "SIZE") || !strcmp(command, "DELE") || !strcmp(command, "RNFR") || !strcmp(command, "RNTO") || !strcmp(command, "STOR")) {
            resolve(&f, argument, path, sizeof(path));
            const char *name = fileName(path);
            char native[600];
            snprintf(native, sizeof(native), "%s/%s", folder, name ? name : "");
            struct stat info;
            if (!name)
                reply(&f, "553 Files can only go into the ROM folder");
            else if (!strcmp(command, "SIZE")) {
                if (stat(native, &info)) reply(&f, "550 No such file");
                else {
                    snprintf(text, sizeof(text), "213 %lld", (long long)info.st_size);
                    reply(&f, text);
                }
            } else if (!strcmp(command, "DELE")) {
                bool ok = !unlink(native);
                diagnosticsTrace("upload: FTP delete %s%s", name, ok ? "" : " (no such file)");
                reply(&f, ok ? "250 Deleted" : "550 No such file");
                if (ok && on_change) on_change();
            } else if (!strcmp(command, "RNFR")) {
                snprintf(f.rename_from, sizeof(f.rename_from), "%s", name);
                reply(&f, stat(native, &info) ? "550 No such file" : "350 Ready");
            } else if (!strcmp(command, "RNTO")) {
                char from[600];
                snprintf(from, sizeof(from), "%s/%s", folder, f.rename_from);
                bool ok = f.rename_from[0] && !rename(from, native);
                diagnosticsTrace("upload: FTP rename %s -> %s%s", f.rename_from, name, ok ? "" : " FAILED");
                reply(&f, ok ? "250 Renamed" : "550 Cannot rename");
                if (ok && on_change) on_change();
            } else {  // STOR
                reply(&f, "150 Send it");
                int fd = dataConnection(&f);
                if (fd >= 0) {
                    bool ok = receiveFile(fd, name, -1, NULL, 0);
                    close(fd);
                    reply(&f, ok ? "226 Stored" : "451 Could not store the file (storage full?)");
                }
            }
        } else if (!strcmp(command, "QUIT")) {
            reply(&f, "221 Bye");
            break;
        } else
            reply(&f, "502 Not here");
    }
done:
    if (f.passive >= 0) close(f.passive);
}

// ---- servers ---------------------------------------------------------------------------------------------------------------------
typedef struct {
    int fd;
    bool ftp;
} Connection;
static void *connectionMain(void *argument) {
    Connection c = *(Connection *)argument;
    free(argument);
    if (c.ftp)
        serveFtp(c.fd);
    else {
        timeouts(c.fd, 30);
        serveHttp(c.fd);
    }
    close(c.fd);
    atomic_fetch_sub(&connections, 1);
    return NULL;
}
static void *acceptMain(void *argument) {
    bool ftp = argument != NULL;
    int listener = listeners[ftp];
    while (!atomic_load(&stopping)) {
        int fd = acceptWithin(listener, 1000);
        if (fd < 0) continue;
        Connection *c = malloc(sizeof(*c));
        pthread_t thread;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 512 * 1024);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        if (c && atomic_load(&connections) < 8) {
            *c = (Connection){fd, ftp};
            atomic_fetch_add(&connections, 1);
            if (pthread_create(&thread, &attributes, connectionMain, c)) {
                atomic_fetch_sub(&connections, 1);
                free(c);
                close(fd);
            }
        } else {
            free(c);
            close(fd);
        }
        pthread_attr_destroy(&attributes);
    }
    return NULL;
}
void uploadServersStart(const char *rom_folder, const char *rom_ftp_path, bool with_ftp, void (*changed)(void)) {
    snprintf(folder, sizeof(folder), "%s", rom_folder);
    snprintf(ftp_path, sizeof(ftp_path), "%s", rom_ftp_path);
    on_change = changed;
    atomic_store(&stopping, false);
    for (int which = 0; which < 2; ++which) {
        if (listeners[which] >= 0 || (which == 1 && !with_ftp)) continue;
        unsigned port = which ? UPLOAD_FTP_PORT : UPLOAD_HTTP_PORT;
        listeners[which] = listenOn(port);
        if (listeners[which] < 0) {
            diagnosticsTrace("upload: %s on port %u could not start (errno %d)", which ? "FTP" : "web page", port, errno);
            continue;
        }
        pthread_create(&acceptors[which], NULL, acceptMain, which ? (void *)1 : NULL);
        atomic_store(which ? &status.ftp_running : &status.http_running, true);
        diagnosticsTrace("upload: %s listening on port %u", which ? "FTP" : "web page", port);
    }
}
void uploadServersStop(void) {
    atomic_store(&stopping, true);
    for (int which = 0; which < 2; ++which) {
        if (listeners[which] < 0) continue;
        pthread_join(acceptors[which], NULL);
        close(listeners[which]);
        listeners[which] = -1;
        atomic_store(which ? &status.ftp_running : &status.http_running, false);
    }
}

unsigned uploadDetectFtp(void) {
    static const unsigned ports[] = {2121, 1337, 21};
    for (unsigned i = 0; i < sizeof(ports) / sizeof(ports[0]); ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        timeouts(fd, 2);
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_port = htons((uint16_t)ports[i]);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        char banner[96] = "";
        bool connected = !connect(fd, (struct sockaddr *)&address, sizeof(address));
        ssize_t got = connected ? recv(fd, banner, sizeof(banner) - 1, 0) : -1;
        close(fd);
        if (got > 0) banner[got] = 0;
        diagnosticsTrace("upload: FTP check on port %u: %s%s", ports[i], connected ? "answered " : "no answer", got > 0 ? banner : "");
        if (got >= 3 && !strncmp(banner, "220", 3) && !strstr(banner, "PokeMMO-Prospero")) return ports[i];
    }
    return 0;
}
