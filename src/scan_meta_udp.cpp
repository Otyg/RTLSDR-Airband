/*
 * scan_meta_udp.cpp
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
#include <string.h>  // strerror()
#include <syslog.h>  // LOG_INFO / LOG_ERR
#include <unistd.h>  // close()

#include <arpa/inet.h>
#include <netdb.h>

#include <string>

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

bool scan_meta_udp_init(scan_meta_udp_data* sdata) {
    sdata->send_socket = -1;
    sdata->dest_sockaddr_len = 0;
    sdata->seq = 0;

    struct addrinfo hints, *result, *rptr;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;
    int error = getaddrinfo(sdata->dest_address, sdata->dest_port, &hints, &result);
    if (error) {
        log(LOG_ERR, "scan_meta_udp: could not resolve %s:%s - %s\n", sdata->dest_address, sdata->dest_port, gai_strerror(error));
        return false;
    }

    for (rptr = result; rptr != NULL; rptr = rptr->ai_next) {
        sdata->send_socket = socket(rptr->ai_family, rptr->ai_socktype, rptr->ai_protocol);
        if (sdata->send_socket == -1) {
            log(LOG_ERR, "scan_meta_udp: socket failed: %s\n", strerror(errno));
            continue;
        }

        if (connect(sdata->send_socket, rptr->ai_addr, rptr->ai_addrlen) == -1) {
            log(LOG_INFO, "scan_meta_udp: connect to %s:%s failed: %s\n", sdata->dest_address, sdata->dest_port, strerror(errno));
            close(sdata->send_socket);
            sdata->send_socket = -1;
            continue;
        }

        sdata->dest_sockaddr = *rptr->ai_addr;
        sdata->dest_sockaddr_len = rptr->ai_addrlen;
        break;
    }
    freeaddrinfo(result);

    if (sdata->send_socket == -1) {
        log(LOG_ERR, "scan_meta_udp: could not set up UDP socket to %s:%s - all addresses failed\n", sdata->dest_address, sdata->dest_port);
        return false;
    }

    log(LOG_INFO, "scan_meta_udp: sending scan metadata to %s:%s\n", sdata->dest_address, sdata->dest_port);
    return true;
}

void scan_meta_udp_write(scan_meta_udp_data* sdata, int device_idx, int freq_hz, char const* label, bool squelch_open) {
    if (sdata->send_socket == -1) {
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

    sendto(sdata->send_socket, msg.data(), msg.size(), MSG_DONTWAIT | MSG_NOSIGNAL, &sdata->dest_sockaddr, sdata->dest_sockaddr_len);
}

void scan_meta_udp_shutdown(scan_meta_udp_data* sdata) {
    if (sdata->send_socket != -1) {
        close(sdata->send_socket);
    }
    sdata->send_socket = -1;
}
