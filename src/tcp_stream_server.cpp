/*
 * tcp_stream_server.cpp
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

#include <cassert>

#include <netdb.h>
#include <sys/socket.h>

#include "rtl_airband.h"

static void close_client(tcp_stream_server_data* sdata) {
    if (sdata->client_socket != -1) {
        close(sdata->client_socket);
        sdata->client_socket = -1;
    }
}

static void close_listener(tcp_stream_server_data* sdata) {
    if (sdata->listen_socket != -1) {
        close(sdata->listen_socket);
        sdata->listen_socket = -1;
    }
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void accept_client_if_available(tcp_stream_server_data* sdata) {
    if (sdata->listen_socket == -1) {
        return;
    }

    for (;;) {
        int client = accept(sdata->listen_socket, NULL, NULL);
        if (client == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            log(LOG_WARNING, "tcp_stream_server: accept failed on %s:%s: %s\n", sdata->bind_address, sdata->bind_port, strerror(errno));
            return;
        }

        if (!set_nonblocking(client)) {
            log(LOG_WARNING, "tcp_stream_server: failed to set nonblocking mode for client on %s:%s\n", sdata->bind_address, sdata->bind_port);
            close(client);
            continue;
        }

        if (sdata->client_socket != -1) {
            close(sdata->client_socket);
        }
        sdata->client_socket = client;
        log(LOG_INFO, "tcp_stream_server: client connected on %s:%s\n", sdata->bind_address, sdata->bind_port);
    }
}

bool tcp_stream_server_init(tcp_stream_server_data* sdata, mix_modes mode, size_t len) {
    if (mode == MM_STEREO) {
        sdata->stereo_buffer_len = len * 2;
        sdata->stereo_buffer = (float*)XCALLOC(sdata->stereo_buffer_len, sizeof(float));
    } else {
        sdata->stereo_buffer_len = 0;
        sdata->stereo_buffer = NULL;
    }

    sdata->listen_socket = -1;
    sdata->client_socket = -1;

    struct addrinfo hints;
    struct addrinfo* result;
    struct addrinfo* rptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int error = getaddrinfo(sdata->bind_address, sdata->bind_port, &hints, &result);
    if (error) {
        log(LOG_ERR, "tcp_stream_server: could not resolve bind address %s:%s - %s\n", sdata->bind_address, sdata->bind_port, gai_strerror(error));
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
        log(LOG_ERR, "tcp_stream_server: could not bind/listen on %s:%s\n", sdata->bind_address, sdata->bind_port);
        return false;
    }

    log(LOG_INFO, "tcp_stream_server: listening for %s 32-bit float at %d Hz on %s:%s\n", mode == MM_MONO ? "Mono" : "Stereo", WAVE_RATE, sdata->bind_address, sdata->bind_port);
    return true;
}

void tcp_stream_server_write(tcp_stream_server_data* sdata, const float* data, size_t len) {
    accept_client_if_available(sdata);

    if (sdata->client_socket == -1) {
        return;
    }

    ssize_t sent = send(sdata->client_socket, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log(LOG_INFO, "tcp_stream_server: client disconnected on %s:%s (%s)\n", sdata->bind_address, sdata->bind_port, strerror(errno));
        close_client(sdata);
    }
}

void tcp_stream_server_write(tcp_stream_server_data* sdata, const float* data_left, const float* data_right, size_t len) {
    if (sdata->client_socket == -1) {
        accept_client_if_available(sdata);
    }
    if (sdata->client_socket == -1) {
        return;
    }

    assert(len * 2 <= sdata->stereo_buffer_len);
    for (size_t i = 0; i < len; ++i) {
        sdata->stereo_buffer[2 * i] = data_left[i];
        sdata->stereo_buffer[2 * i + 1] = data_right[i];
    }
    tcp_stream_server_write(sdata, sdata->stereo_buffer, len * 2);
}

void tcp_stream_server_shutdown(tcp_stream_server_data* sdata) {
    close_client(sdata);
    close_listener(sdata);
}
