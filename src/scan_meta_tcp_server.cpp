/*
 * scan_meta_tcp_server.cpp
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <string>

#include <netdb.h>
#include <sys/socket.h>

#include "rtl_airband.h"

static void append_json_escaped_string(std::string* dst, char const* src) {
    if (src == NULL) {
        return;
    }
    for (char const* p = src; *p != '\0'; ++p) {
        switch (*p) {
            case '\"':
                *dst += "\\\"";
                break;
            case '\\':
                *dst += "\\\\";
                break;
            case '\b':
                *dst += "\\b";
                break;
            case '\f':
                *dst += "\\f";
                break;
            case '\n':
                *dst += "\\n";
                break;
            case '\r':
                *dst += "\\r";
                break;
            case '\t':
                *dst += "\\t";
                break;
            default:
                *dst += *p;
                break;
        }
    }
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_client(scan_meta_tcp_server_data* sdata) {
    if (sdata->client_socket != -1) {
        close(sdata->client_socket);
        sdata->client_socket = -1;
        sdata->hello_sent = false;
        sdata->hello_offset = 0;
    }
}

static void close_listener(scan_meta_tcp_server_data* sdata) {
    if (sdata->listen_socket != -1) {
        close(sdata->listen_socket);
        sdata->listen_socket = -1;
    }
}

static void accept_client_if_available(scan_meta_tcp_server_data* sdata) {
    if (sdata->listen_socket == -1) {
        return;
    }

    for (;;) {
        int client = accept(sdata->listen_socket, NULL, NULL);
        if (client == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            log(LOG_WARNING, "scan_meta_tcp_server: accept failed on %s:%s: %s\n", sdata->bind_address, sdata->bind_port, strerror(errno));
            return;
        }

        if (!set_nonblocking(client)) {
            log(LOG_WARNING, "scan_meta_tcp_server: failed to set nonblocking mode for client on %s:%s\n", sdata->bind_address, sdata->bind_port);
            close(client);
            continue;
        }

        if (sdata->client_socket != -1) {
            close(sdata->client_socket);
        }
        sdata->client_socket = client;
        sdata->hello_sent = false;
        sdata->hello_offset = 0;
        log(LOG_INFO, "scan_meta_tcp_server: client connected on %s:%s\n", sdata->bind_address, sdata->bind_port);
    }
}

static void send_hello_if_needed(scan_meta_tcp_server_data* sdata) {
    if (sdata->client_socket == -1 || sdata->hello_sent || sdata->channel_list_json == NULL) {
        return;
    }

    size_t msglen = strlen(sdata->channel_list_json);
    if (sdata->hello_offset >= msglen) {
        sdata->hello_sent = true;
        return;
    }

    ssize_t sent = send(sdata->client_socket, sdata->channel_list_json + sdata->hello_offset, msglen - sdata->hello_offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) {
        sdata->hello_offset += (size_t)sent;
        if (sdata->hello_offset >= msglen) {
            sdata->hello_sent = true;
        }
    } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log(LOG_INFO, "scan_meta_tcp_server: client disconnected during hello on %s:%s (%s)\n", sdata->bind_address, sdata->bind_port, strerror(errno));
        close_client(sdata);
    }
}

bool scan_meta_tcp_server_init(scan_meta_tcp_server_data* sdata) {
    sdata->seq = 0;
    sdata->listen_socket = -1;
    sdata->client_socket = -1;
    sdata->hello_sent = false;
    sdata->hello_offset = 0;

    struct addrinfo hints;
    struct addrinfo* result;
    struct addrinfo* rptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int error = getaddrinfo(sdata->bind_address, sdata->bind_port, &hints, &result);
    if (error) {
        log(LOG_ERR, "scan_meta_tcp_server: could not resolve bind address %s:%s - %s\n", sdata->bind_address, sdata->bind_port, gai_strerror(error));
        return false;
    }

    for (rptr = result; rptr != NULL; rptr = rptr->ai_next) {
        int fd = socket(rptr->ai_family, rptr->ai_socktype, rptr->ai_protocol);
        if (fd == -1) {
            continue;
        }

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(fd, rptr->ai_addr, rptr->ai_addrlen) != 0) {
            close(fd);
            continue;
        }

        if (listen(fd, 4) != 0) {
            close(fd);
            continue;
        }

        if (!set_nonblocking(fd)) {
            close(fd);
            continue;
        }

        sdata->listen_socket = fd;
        break;
    }

    freeaddrinfo(result);

    if (sdata->listen_socket == -1) {
        log(LOG_ERR, "scan_meta_tcp_server: could not bind/listen on %s:%s\n", sdata->bind_address, sdata->bind_port);
        return false;
    }

    log(LOG_INFO, "scan_meta_tcp_server: listening for scanner metadata on %s:%s\n", sdata->bind_address, sdata->bind_port);
    return true;
}

void scan_meta_tcp_server_write(scan_meta_tcp_server_data* sdata, int device_idx, int freq_hz, char const* label, bool squelch_open) {
    accept_client_if_available(sdata);
    send_hello_if_needed(sdata);

    if (sdata->client_socket == -1 || !sdata->hello_sent) {
        return;
    }

    std::string msg = "{\"v\":1,\"seq\":";
    msg += std::to_string(sdata->seq++);
    msg += ",\"device\":";
    msg += std::to_string(device_idx);
    msg += ",\"freq_hz\":";
    msg += std::to_string(freq_hz);
    msg += ",\"squelch_open\":";
    msg += (squelch_open ? "true" : "false");
    msg += ",\"label\":\"";
    append_json_escaped_string(&msg, label);
    msg += "\"}\n";

    ssize_t sent = send(sdata->client_socket, msg.data(), msg.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log(LOG_INFO, "scan_meta_tcp_server: client disconnected on %s:%s (%s)\n", sdata->bind_address, sdata->bind_port, strerror(errno));
        close_client(sdata);
    }
}

void scan_meta_tcp_server_shutdown(scan_meta_tcp_server_data* sdata) {
    close_client(sdata);
    close_listener(sdata);
}
