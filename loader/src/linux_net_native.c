// SPDX-License-Identifier: MIT AND GPL-3.0-or-later
// Adapted from PokeMMO-NX (https://github.com/Petit-Prince-dev/PokeMMO-NX, source/linux_net_native.c)
// Copyright (c) Petit_Prince, MIT license (LICENSES/PokeMMO-NX-MIT.txt). PS5 changes: PokeMMO-Prospero contributors.
// Changes: the native side is the platform's own BSD sockets. linux_net_translate.c produces FreeBSD numbers and layouts (as
// Horizon's BSD service uses); they are mapped here, by name, to the platform's (identical on the PS5, different on a PC).
// Name resolution goes through platformResolveIPv4 (the PS5's sceNetResolver: getaddrinfo crashes inside a PS5 title).
#include "linux_net_native.h"
#include "diagnostics.h"
#include "linux_abi.h"
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static atomic_uint traced;
static void trace(const char *format, ...) {
    if (atomic_fetch_add(&traced, 1) >= 200) return;
    va_list args;
    va_start(args, format);
    diagnosticsTraceV(format, args);
    va_end(args);
}
static int failure(const char *operation, int *error) {
    int native = errno;
    *error = linuxNetErrnoFromNative(native);
    trace("net.%s=ERROR native_errno=%d linux_errno=%d", operation, native, *error);
    return -1;
}

// ---- BSD numbers (what linux_net_translate.c produces) to native ones -------------------------------------------------------------
static int nativeLevel(int level) { return level == BSD_SOL_SOCKET ? SOL_SOCKET : level; }  // IPPROTO_TCP is 6 everywhere
static int nativeOption(int level, int name) {
    if (level == BSD_IPPROTO_TCP) return name == BSD_TCP_NODELAY ? TCP_NODELAY : -1;
    switch (name) {
        case BSD_SO_REUSEADDR: return SO_REUSEADDR;
        case BSD_SO_KEEPALIVE: return SO_KEEPALIVE;
        case BSD_SO_BROADCAST: return SO_BROADCAST;
        case BSD_SO_LINGER: return SO_LINGER;
        case BSD_SO_REUSEPORT: return SO_REUSEPORT;
        case BSD_SO_SNDBUF: return SO_SNDBUF;
        case BSD_SO_RCVBUF: return SO_RCVBUF;
        case BSD_SO_SNDTIMEO: return SO_SNDTIMEO;
        case BSD_SO_RCVTIMEO: return SO_RCVTIMEO;
        case BSD_SO_ERROR: return SO_ERROR;
        case BSD_SO_TYPE: return SO_TYPE;
        default: return -1;
    }
}
static int nativeMessageFlags(int flags) {
    int result = 0;
    if (flags & BSD_MSG_PEEK) result |= MSG_PEEK;
    if (flags & BSD_MSG_DONTWAIT) result |= MSG_DONTWAIT;
    if (flags & BSD_MSG_WAITALL) result |= MSG_WAITALL;
#ifdef MSG_NOSIGNAL
    result |= MSG_NOSIGNAL;  // a closed peer must not kill the process (Linux has SIGPIPE on by default)
#endif
    return result;
}
static short nativePoll(int events) {
    return (short)((events & BSD_POLLIN ? POLLIN : 0) | (events & BSD_POLLPRI ? POLLPRI : 0) | (events & BSD_POLLOUT ? POLLOUT : 0) |
                   (events & BSD_POLLERR ? POLLERR : 0) | (events & BSD_POLLHUP ? POLLHUP : 0) | (events & BSD_POLLNVAL ? POLLNVAL : 0));
}
static int16_t bsdPoll(short events) {
    return (int16_t)((events & POLLIN ? BSD_POLLIN : 0) | (events & POLLPRI ? BSD_POLLPRI : 0) | (events & POLLOUT ? BSD_POLLOUT : 0) |
                     (events & POLLERR ? BSD_POLLERR : 0) | (events & POLLHUP ? BSD_POLLHUP : 0) | (events & POLLNVAL ? BSD_POLLNVAL : 0));
}
static struct sockaddr_in toNative(const BsdSockaddrIn *address) {
    struct sockaddr_in native;
    memset(&native, 0, sizeof(native));
    native.sin_family = AF_INET;
    native.sin_port = address->port;
    native.sin_addr.s_addr = address->address;
    return native;
}
static void fromNative(const struct sockaddr_in *native, BsdSockaddrIn *address) {
    memset(address, 0, sizeof(*address));
    address->length = sizeof(*address);
    address->family = BSD_AF_INET;
    address->port = native->sin_port;
    address->address = native->sin_addr.s_addr;
}
// Socket option values: integers and timeouts have the same meaning; struct timeval and struct linger are 16 and 8 bytes on both.

int linuxNetNativeSocket(int type, int protocol, int *error) {
    int native_type = type == BSD_SOCK_DGRAM ? SOCK_DGRAM : SOCK_STREAM;
    int handle = socket(AF_INET, native_type, protocol);
    trace("net.socket=%s handle=%d type=%d", handle < 0 ? "ERROR" : "OK", handle, type);
    if (handle < 0) return failure("socket", error);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return handle;
}
int linuxNetNativeClose(int handle) {
    int result = close(handle);
    trace("net.close handle=%d result=%d", handle, result);
    return result;
}
int linuxNetNativeSetNonblocking(int handle, bool enable, int *error) {
    int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0) return failure("fcntl_get", error);
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(handle, F_SETFL, flags) < 0 ? failure("fcntl_set", error) : 0;
}
int linuxNetNativeConnect(int handle, const BsdSockaddrIn *address, int *error) {
    struct sockaddr_in native = toNative(address);
    int result = connect(handle, (const struct sockaddr *)&native, sizeof(native));
    uint32_t host = __builtin_bswap32(address->address);
    trace("net.connect handle=%d address=%u.%u.%u.%u port=%u result=%d native_errno=%d", handle, host >> 24, (host >> 16) & 255, (host >> 8) & 255, host & 255,
          (unsigned)__builtin_bswap16(address->port), result, result < 0 ? errno : 0);
    return result < 0 ? failure("connect", error) : 0;
}
int linuxNetNativeBind(int handle, const BsdSockaddrIn *address, int *error) {
    struct sockaddr_in native = toNative(address);
    return bind(handle, (const struct sockaddr *)&native, sizeof(native)) < 0 ? failure("bind", error) : 0;
}
int linuxNetNativeListen(int handle, int backlog, int *error) { return listen(handle, backlog) < 0 ? failure("listen", error) : 0; }
int linuxNetNativeAccept(int handle, BsdSockaddrIn *address, int *error) {
    struct sockaddr_in native;
    socklen_t length = sizeof(native);
    int accepted = accept(handle, (struct sockaddr *)&native, &length);
    if (accepted < 0) return failure("accept", error);
    fromNative(&native, address);
    return accepted;
}
int linuxNetNativeName(int handle, bool peer, BsdSockaddrIn *address, int *error) {
    struct sockaddr_in native;
    socklen_t length = sizeof(native);
    int result = peer ? getpeername(handle, (struct sockaddr *)&native, &length) : getsockname(handle, (struct sockaddr *)&native, &length);
    if (result < 0) return failure(peer ? "getpeername" : "getsockname", error);
    fromNative(&native, address);
    return 0;
}
int linuxNetNativeSetsockopt(int handle, int level, int name, const void *value, unsigned length, int *error) {
    int native_name = nativeOption(level, name);
    if (native_name < 0) {
        *error = LINUX_ENOPROTOOPT;
        return -1;
    }
    int result = setsockopt(handle, nativeLevel(level), native_name, value, length);
    trace("net.setsockopt handle=%d level=0x%x name=0x%x result=%d", handle, level, name, result);
    return result < 0 ? failure("setsockopt", error) : 0;
}
int linuxNetNativeGetsockopt(int handle, int level, int name, void *value, unsigned *length, int *error) {
    int native_name = nativeOption(level, name);
    if (native_name < 0) {
        *error = LINUX_ENOPROTOOPT;
        return -1;
    }
    socklen_t native_length = *length;
    int result = getsockopt(handle, nativeLevel(level), native_name, value, &native_length);
    if (result < 0) return failure("getsockopt", error);
    *length = native_length;
    if (name == BSD_SO_ERROR && native_length >= sizeof(int)) {  // the pending error is a native errno: the guest wants a Linux one
        int value_error;
        memcpy(&value_error, value, sizeof(value_error));
        value_error = value_error ? linuxNetErrnoFromNative(value_error) : 0;
        memcpy(value, &value_error, sizeof(value_error));
    }
    if (name == BSD_SO_TYPE && native_length >= sizeof(int)) {
        int type;
        memcpy(&type, value, sizeof(type));
        type = type == SOCK_DGRAM ? BSD_SOCK_DGRAM : BSD_SOCK_STREAM;
        memcpy(value, &type, sizeof(type));
    }
    return 0;
}
int linuxNetNativeShutdown(int handle, int how, int *error) { return shutdown(handle, how) < 0 ? failure("shutdown", error) : 0; }
int64_t linuxNetNativeSend(int handle, const void *buffer, size_t count, int flags, const BsdSockaddrIn *to, int *error) {
    int native_flags = nativeMessageFlags(flags);
    ssize_t result;
    if (to) {
        struct sockaddr_in native = toNative(to);
        result = sendto(handle, buffer, count, native_flags, (const struct sockaddr *)&native, sizeof(native));
    } else
        result = send(handle, buffer, count, native_flags);
    return result < 0 ? failure("send", error) : (int64_t)result;
}
int64_t linuxNetNativeRecv(int handle, void *buffer, size_t count, int flags, BsdSockaddrIn *from, int *error) {
    int native_flags = nativeMessageFlags(flags);
    ssize_t result;
    if (from) {
        struct sockaddr_in native;
        socklen_t length = sizeof(native);
        result = recvfrom(handle, buffer, count, native_flags, (struct sockaddr *)&native, &length);
        if (result >= 0) fromNative(&native, from);
    } else
        result = recv(handle, buffer, count, native_flags);
    return result < 0 ? failure("recv", error) : (int64_t)result;
}
int linuxNetNativePoll(LinuxNetPollEntry *entries, unsigned count, int timeout_ms, int *error) {
    if (!count) {
        if (timeout_ms > 0) platformSleepNs((uint64_t)timeout_ms * 1000000u);
        return 0;
    }
    struct pollfd native[64];
    if (count > 64) {
        *error = LINUX_EINVAL;
        return -1;
    }
    for (unsigned i = 0; i < count; ++i) {
        native[i].fd = entries[i].handle;
        native[i].events = nativePoll(entries[i].events);
        native[i].revents = 0;
    }
    int ready = poll(native, count, timeout_ms);
    if (ready < 0) return failure("poll", error);
    for (unsigned i = 0; i < count; ++i) entries[i].revents = bsdPoll(native[i].revents);
    return ready;
}

bool linuxNetNativeLocalNetwork(uint32_t *address, uint32_t *netmask) {
    (void)address;
    (void)netmask;
    return false;
}

// Only numeric services (ports) are understood; Java passes none (it sets the port itself).
int linuxNetNativeResolve(const char *node, const char *service, int flags, int socktype, int protocol, LinuxNetResolved *result) {
    memset(result, 0, sizeof(*result));
    uint16_t port = 0;
    if (service && *service) {
        char *end = NULL;
        unsigned long value = strtoul(service, &end, 10);
        if (*end || value > 65535) return LINUX_EAI_SERVICE;
        port = __builtin_bswap16((uint16_t)value);
    }
    uint32_t address = 0;
    int code = 0;
    if (!node) {
        address = (flags & LINUX_AI_PASSIVE) ? 0 : __builtin_bswap32(0x7f000001u);
    } else {
        unsigned a, b, c, d;
        char tail;
        if (sscanf(node, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
            address = __builtin_bswap32((a << 24) | (b << 16) | (c << 8) | d);
        else if (flags & LINUX_AI_NUMERICHOST)
            code = LINUX_EAI_NONAME;
        else if (!strcmp(node, "localhost"))
            address = __builtin_bswap32(0x7f000001u);
        else
            code = platformResolveIPv4(node, &address);
    }
    trace("net.resolve node=%s service=%s result=%d", node ? node : "(null)", service ? service : "(null)", code);
    if (code) return code;
    // One entry per socket type asked for (both when none was).
    int types[2] = {socktype ? socktype : BSD_SOCK_STREAM, BSD_SOCK_DGRAM};
    unsigned type_count = socktype ? 1 : 2;
    for (unsigned i = 0; i < type_count; ++i) {
        result->entries[result->count].address = address;
        result->entries[result->count].port = port;
        result->entries[result->count].socktype = types[i];
        result->entries[result->count].protocol = protocol ? protocol : (types[i] == BSD_SOCK_STREAM ? 6 : 17);
        ++result->count;
    }
    if (flags & LINUX_AI_CANONNAME) snprintf(result->canonical_name, sizeof(result->canonical_name), "%s", node ? node : "");
    return 0;
}
