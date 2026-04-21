/*
 * udp_stream_server.cpp
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
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <cassert>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>

#include "rtl_airband.h"

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_socket(udp_stream_server_data* sdata) {
    if (sdata->socket_fd != -1) {
        close(sdata->socket_fd);
        sdata->socket_fd = -1;
    }
}

static void update_client_if_available(udp_stream_server_data* sdata) {
    if (sdata->socket_fd == -1) {
        return;
    }

    char probe[1];
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    for (;;) {
        ssize_t rc = recvfrom(sdata->socket_fd, probe, sizeof(probe), MSG_DONTWAIT, (struct sockaddr*)&addr, &addrlen);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            log(LOG_WARNING, "udp_stream_server: recvfrom failed on %s:%s: %s\n", sdata->bind_address, sdata->bind_port, strerror(errno));
            return;
        }

        bool client_changed = !sdata->has_client || sdata->client_sockaddr_len != addrlen || memcmp(&sdata->client_sockaddr, &addr, addrlen) != 0;
        sdata->has_client = true;
        sdata->client_sockaddr = addr;
        sdata->client_sockaddr_len = addrlen;

        if (client_changed) {
            char host[NI_MAXHOST];
            char service[NI_MAXSERV];
            int err = getnameinfo((struct sockaddr*)&addr, addrlen, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
            if (err == 0) {
                log(LOG_INFO, "udp_stream_server: client registered on %s:%s from %s:%s\n", sdata->bind_address, sdata->bind_port, host, service);
            } else {
                log(LOG_INFO, "udp_stream_server: client registered on %s:%s\n", sdata->bind_address, sdata->bind_port);
            }
        }
        addrlen = sizeof(addr);
    }
}

bool udp_stream_server_init(udp_stream_server_data* sdata, mix_modes mode, size_t len) {
    if (mode == MM_STEREO) {
        sdata->stereo_buffer_len = len * 2;
        sdata->stereo_buffer = (float*)XCALLOC(sdata->stereo_buffer_len, sizeof(float));
    } else {
        sdata->stereo_buffer_len = 0;
        sdata->stereo_buffer = NULL;
    }

    sdata->socket_fd = -1;
    sdata->has_client = false;
    sdata->client_sockaddr_len = 0;

    struct addrinfo hints;
    struct addrinfo* result;
    struct addrinfo* rptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    int error = getaddrinfo(sdata->bind_address, sdata->bind_port, &hints, &result);
    if (error) {
        log(LOG_ERR, "udp_stream_server: could not resolve bind address %s:%s - %s\n", sdata->bind_address, sdata->bind_port, gai_strerror(error));
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

        if (!set_nonblocking(fd)) {
            close(fd);
            continue;
        }

        sdata->socket_fd = fd;
        break;
    }

    freeaddrinfo(result);

    if (sdata->socket_fd == -1) {
        log(LOG_ERR, "udp_stream_server: could not bind on %s:%s\n", sdata->bind_address, sdata->bind_port);
        return false;
    }

    log(LOG_INFO, "udp_stream_server: listening for %s 32-bit float at %d Hz on %s:%s\n", mode == MM_MONO ? "Mono" : "Stereo", WAVE_RATE, sdata->bind_address, sdata->bind_port);
    return true;
}

void udp_stream_server_write(udp_stream_server_data* sdata, const float* data, size_t len) {
    update_client_if_available(sdata);

    if (!sdata->has_client || sdata->socket_fd == -1) {
        return;
    }

    ssize_t sent = sendto(sdata->socket_fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr*)&sdata->client_sockaddr, sdata->client_sockaddr_len);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log(LOG_INFO, "udp_stream_server: send failed on %s:%s (%s)\n", sdata->bind_address, sdata->bind_port, strerror(errno));
    }
}

void udp_stream_server_write(udp_stream_server_data* sdata, const float* data_left, const float* data_right, size_t len) {
    if (!sdata->has_client) {
        update_client_if_available(sdata);
    }
    if (!sdata->has_client) {
        return;
    }

    assert(len * 2 <= sdata->stereo_buffer_len);
    for (size_t i = 0; i < len; ++i) {
        sdata->stereo_buffer[2 * i] = data_left[i];
        sdata->stereo_buffer[2 * i + 1] = data_right[i];
    }
    udp_stream_server_write(sdata, sdata->stereo_buffer, len * 2);
}

void udp_stream_server_shutdown(udp_stream_server_data* sdata) {
    sdata->has_client = false;
    sdata->client_sockaddr_len = 0;
    close_socket(sdata);
}
