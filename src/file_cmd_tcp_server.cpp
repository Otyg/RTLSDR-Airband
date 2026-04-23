/*
 * file_cmd_tcp_server.cpp
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

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

static void queue_response(file_cmd_tcp_server_data* sdata, std::string const& response) {
    if (response.empty() || sdata->client_socket == -1) {
        return;
    }
    sdata->send_buffer += response;
}

static void flush_send_buffer(file_cmd_tcp_server_data* sdata) {
    if (sdata->client_socket == -1) {
        return;
    }

    while (sdata->send_offset < sdata->send_buffer.size()) {
        ssize_t sent = send(sdata->client_socket, sdata->send_buffer.data() + sdata->send_offset, sdata->send_buffer.size() - sdata->send_offset, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            sdata->send_offset += (size_t)sent;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        log(LOG_INFO, "file_cmd_tcp_server: client disconnected on %s:%s (%s)\n", sdata->bind_address, sdata->bind_port, strerror(errno));
        close(sdata->client_socket);
        sdata->client_socket = -1;
        sdata->recv_buffer.clear();
        sdata->send_buffer.clear();
        sdata->send_offset = 0;
        return;
    }

    sdata->send_buffer.clear();
    sdata->send_offset = 0;
}

static std::string trim(std::string const& value) {
    size_t begin = 0;
    while (begin < value.size() && isspace((unsigned char)value[begin])) {
        begin++;
    }

    size_t end = value.size();
    while (end > begin && isspace((unsigned char)value[end - 1])) {
        end--;
    }
    return value.substr(begin, end - begin);
}

static std::string to_upper_ascii(std::string const& value) {
    std::string ret = value;
    for (size_t i = 0; i < ret.size(); ++i) {
        ret[i] = (char)toupper((unsigned char)ret[i]);
    }
    return ret;
}

static bool is_directory(std::string const& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static bool is_regular_file(std::string const& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

static std::string join_path(std::string const& base, std::string const& name) {
    if (base.empty()) {
        return name;
    }
    if (base[base.size() - 1] == '/') {
        return base + name;
    }
    return base + "/" + name;
}

static bool ends_with_ignore_case(std::string const& value, std::string const& suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }
    size_t offset = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = (char)tolower((unsigned char)value[offset + i]);
        char b = (char)tolower((unsigned char)suffix[i]);
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool resolve_realpath(std::string const& path, std::string* resolved) {
    char tmp[PATH_MAX];
    if (realpath(path.c_str(), tmp) == NULL) {
        return false;
    }
    *resolved = tmp;
    return true;
}

static bool is_path_under_dir(std::string const& path, std::string const& dir) {
    if (path.size() < dir.size()) {
        return false;
    }
    if (path.compare(0, dir.size(), dir) != 0) {
        return false;
    }
    if (path.size() == dir.size()) {
        return true;
    }
    return dir.back() == '/' || path[dir.size()] == '/';
}

static bool is_file_allowed(file_cmd_tcp_server_data* sdata, std::string const& file_path) {
    std::string resolved_file;
    if (!resolve_realpath(file_path, &resolved_file)) {
        return false;
    }

    for (size_t i = 0; i < sdata->directories.size(); ++i) {
        std::string resolved_dir;
        if (!resolve_realpath(sdata->directories[i], &resolved_dir)) {
            continue;
        }
        if (is_path_under_dir(resolved_file, resolved_dir)) {
            return true;
        }
    }
    return false;
}

static void collect_files_recursive(std::string const& dir_path, std::vector<std::string>* files) {
    DIR* dir = opendir(dir_path.c_str());
    if (dir == NULL) {
        return;
    }

    struct dirent* ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }

        std::string full_path = join_path(dir_path, ent->d_name);
        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            collect_files_recursive(full_path, files);
        } else if (S_ISREG(st.st_mode)) {
            files->push_back(full_path);
        }
    }

    closedir(dir);
}

static std::string build_list_files_response(file_cmd_tcp_server_data* sdata) {
    std::vector<std::string> files;
    files.reserve(256);

    for (size_t i = 0; i < sdata->directories.size(); ++i) {
        if (!is_directory(sdata->directories[i])) {
            continue;
        }
        collect_files_recursive(sdata->directories[i], &files);
    }

    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    std::string response = "OK LIST_FILES ";
    response += std::to_string(files.size());
    response += "\n";

    for (size_t i = 0; i < files.size(); ++i) {
        response += files[i];
        response += "\n";
    }

    response += ".\n";
    return response;
}

static void clear_decoder_process(file_cmd_tcp_server_data* sdata) {
    if (sdata->playback_pipe_fd != -1) {
        close(sdata->playback_pipe_fd);
        sdata->playback_pipe_fd = -1;
    }
    if (sdata->playback_stderr_fd != -1) {
        close(sdata->playback_stderr_fd);
        sdata->playback_stderr_fd = -1;
    }

    if (sdata->playback_decoder_pid > 0) {
        int status = 0;
        pid_t ret = waitpid((pid_t)sdata->playback_decoder_pid, &status, WNOHANG);
        if (ret == 0) {
            kill((pid_t)sdata->playback_decoder_pid, SIGTERM);
            waitpid((pid_t)sdata->playback_decoder_pid, &status, 0);
        }
    }

    sdata->playback_decoder_pid = -1;
    sdata->playback_decoder_eof = false;
    sdata->playback_pcm16_buffer.clear();
    sdata->playback_decoder_stderr.clear();
}

static void stop_playback(file_cmd_tcp_server_data* sdata) {
    sdata->playback_active = false;
    sdata->playback_loop = false;
    sdata->playback_waiting_for_first_chunk = false;
    sdata->playback_file_path.clear();
    clear_decoder_process(sdata);
}

static bool start_decoder_process(file_cmd_tcp_server_data* sdata, std::string const& file_path) {
    int pipefd[2] = {-1, -1};
    int errpipe[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        log(LOG_WARNING, "file_cmd_tcp_server: pipe() failed: %s\n", strerror(errno));
        return false;
    }
    if (pipe(errpipe) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        log(LOG_WARNING, "file_cmd_tcp_server: stderr pipe() failed: %s\n", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(errpipe[0]);
        close(errpipe[1]);
        log(LOG_WARNING, "file_cmd_tcp_server: fork() failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        close(errpipe[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }
        if (dup2(errpipe[1], STDERR_FILENO) == -1) {
            _exit(127);
        }
        close(pipefd[1]);
        close(errpipe[1]);

        char sample_rate[16];
        snprintf(sample_rate, sizeof(sample_rate), "%d", WAVE_RATE);

        execl("/usr/bin/ffmpeg", "ffmpeg", "-v", "error", "-nostdin", "-i", file_path.c_str(), "-map", "0:a:0", "-vn", "-sn", "-dn", "-f", "s16le", "-ac", "1", "-ar", sample_rate, "pipe:1", (char*)NULL);
        execlp("ffmpeg", "ffmpeg", "-v", "error", "-nostdin", "-i", file_path.c_str(), "-map", "0:a:0", "-vn", "-sn", "-dn", "-f", "s16le", "-ac", "1", "-ar", sample_rate, "pipe:1", (char*)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    close(errpipe[1]);
    if (!set_nonblocking(pipefd[0])) {
        close(pipefd[0]);
        close(errpipe[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return false;
    }
    if (!set_nonblocking(errpipe[0])) {
        close(pipefd[0]);
        close(errpipe[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return false;
    }

    sdata->playback_pipe_fd = pipefd[0];
    sdata->playback_stderr_fd = errpipe[0];
    sdata->playback_decoder_pid = (int)pid;
    sdata->playback_decoder_eof = false;
    sdata->playback_pcm16_buffer.clear();
    sdata->playback_decoder_stderr.clear();
    sdata->playback_active = true;
    return true;
}

static bool start_playback(file_cmd_tcp_server_data* sdata, std::string const& file_path, std::string* error_message) {
    if (sdata->playback_udp_stream == NULL && sdata->playback_udp_stream_server == NULL) {
        *error_message = "ERR no udp_stream or udp_stream_server output configured on this channel\n";
        return false;
    }

    if (sdata->playback_udp_stream != NULL && sdata->playback_udp_stream->send_socket == -1) {
        *error_message = "ERR udp_stream output is not connected\n";
        return false;
    }
    if (sdata->playback_udp_stream_server != NULL && sdata->playback_udp_stream_server->socket_fd == -1) {
        *error_message = "ERR udp_stream_server output is not available\n";
        return false;
    }

    if (!is_regular_file(file_path)) {
        *error_message = "ERR file not found\n";
        return false;
    }

    if (!is_file_allowed(sdata, file_path)) {
        *error_message = "ERR file outside configured recording directories\n";
        return false;
    }

    if (!ends_with_ignore_case(file_path, ".mp3") && !ends_with_ignore_case(file_path, ".flac")) {
        *error_message = "ERR unsupported file extension (use .mp3 or .flac)\n";
        return false;
    }

    stop_playback(sdata);
    if (!start_decoder_process(sdata, file_path)) {
        *error_message = "ERR failed to start decoder (ffmpeg required)\n";
        return false;
    }

    sdata->playback_file_path = file_path;
    sdata->playback_waiting_for_first_chunk = true;
    if (sdata->playback_udp_stream_server != NULL) {
        log(LOG_INFO, "file_cmd_tcp_server: playback target is udp_stream_server %s:%s\n", sdata->playback_udp_stream_server->bind_address, sdata->playback_udp_stream_server->bind_port);
    } else if (sdata->playback_udp_stream != NULL) {
        log(LOG_INFO, "file_cmd_tcp_server: playback target is udp_stream %s:%s\n", sdata->playback_udp_stream->dest_address, sdata->playback_udp_stream->dest_port);
    }
    return true;
}

static void send_playback_samples(file_cmd_tcp_server_data* sdata, float const* mono, size_t sample_count) {
    if (sdata->playback_waiting_for_first_chunk) {
        log(LOG_INFO, "file_cmd_tcp_server: first decoded audio chunk ready (%zu samples)\n", sample_count);
    }
    size_t const bytes = sample_count * sizeof(float);
    if (sdata->playback_mode == MM_MONO) {
        if (sdata->playback_udp_stream != NULL) {
            udp_stream_write(sdata->playback_udp_stream, mono, bytes);
        } else {
            udp_stream_server_write(sdata->playback_udp_stream_server, mono, bytes);
        }
    } else {
        if (sdata->playback_udp_stream != NULL) {
            udp_stream_write(sdata->playback_udp_stream, mono, mono, bytes);
        } else {
            udp_stream_server_write(sdata->playback_udp_stream_server, mono, mono, bytes);
        }
    }
}

static void send_noise_batch(file_cmd_tcp_server_data* sdata, size_t sample_count) {
    // Fill startup gap with low-level white noise until first decoded chunk is ready.
    std::vector<float> noise(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        sdata->playback_noise_state = sdata->playback_noise_state * 1664525u + 1013904223u;
        float normalized = ((float)(sdata->playback_noise_state & 0xFFFFu) / 32767.5f) - 1.0f;
        noise[i] = normalized * 0.02f;
    }
    send_playback_samples(sdata, noise.data(), sample_count);
}

static void read_decoder_stderr(file_cmd_tcp_server_data* sdata) {
    if (sdata->playback_stderr_fd == -1) {
        return;
    }
    for (;;) {
        char buf[512];
        ssize_t n = read(sdata->playback_stderr_fd, buf, sizeof(buf));
        if (n > 0) {
            sdata->playback_decoder_stderr.append(buf, (size_t)n);
            if (sdata->playback_decoder_stderr.size() > 4096) {
                sdata->playback_decoder_stderr.erase(0, sdata->playback_decoder_stderr.size() - 4096);
            }
            continue;
        }
        if (n == 0) {
            close(sdata->playback_stderr_fd);
            sdata->playback_stderr_fd = -1;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close(sdata->playback_stderr_fd);
        sdata->playback_stderr_fd = -1;
        return;
    }
}

static void pump_playback(file_cmd_tcp_server_data* sdata) {
    if (!sdata->playback_active || (sdata->playback_udp_stream == NULL && sdata->playback_udp_stream_server == NULL)) {
        return;
    }

    size_t const target_samples = (size_t)WAVE_BATCH;
    size_t const target_bytes = target_samples * sizeof(int16_t);
    read_decoder_stderr(sdata);

    while (sdata->playback_pcm16_buffer.size() < target_bytes && !sdata->playback_decoder_eof && sdata->playback_pipe_fd != -1) {
        unsigned char tmp[4096];
        ssize_t bytes_read = read(sdata->playback_pipe_fd, tmp, sizeof(tmp));
        if (bytes_read > 0) {
            sdata->playback_pcm16_buffer.insert(sdata->playback_pcm16_buffer.end(), tmp, tmp + bytes_read);
            continue;
        }

        if (bytes_read == 0) {
            close(sdata->playback_pipe_fd);
            sdata->playback_pipe_fd = -1;
            sdata->playback_decoder_eof = true;
            if (sdata->playback_decoder_pid > 0) {
                int status = 0;
                pid_t ret = waitpid((pid_t)sdata->playback_decoder_pid, &status, WNOHANG);
                if (ret == (pid_t)sdata->playback_decoder_pid) {
                    read_decoder_stderr(sdata);
                    if (WIFEXITED(status)) {
                        log(LOG_INFO, "file_cmd_tcp_server: decoder exited with status %d for %s\n", WEXITSTATUS(status), sdata->playback_file_path.c_str());
                    } else if (WIFSIGNALED(status)) {
                        log(LOG_INFO, "file_cmd_tcp_server: decoder killed by signal %d for %s\n", WTERMSIG(status), sdata->playback_file_path.c_str());
                    }
                    sdata->playback_decoder_pid = -1;
                }
            }
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        log(LOG_WARNING, "file_cmd_tcp_server: decoder read failed: %s\n", strerror(errno));
        stop_playback(sdata);
        queue_response(sdata, "ERR playback failed\n");
        return;
    }

    size_t available_samples = sdata->playback_pcm16_buffer.size() / sizeof(int16_t);
    if (available_samples == 0) {
        if (sdata->playback_decoder_eof) {
            if (sdata->playback_waiting_for_first_chunk) {
                log(LOG_INFO, "file_cmd_tcp_server: decoder produced no samples for %s\n", sdata->playback_file_path.c_str());
                if (!sdata->playback_decoder_stderr.empty()) {
                    log(LOG_INFO, "file_cmd_tcp_server: decoder stderr: %s\n", sdata->playback_decoder_stderr.c_str());
                }
                stop_playback(sdata);
                queue_response(sdata, "ERR playback failed\n");
                return;
            }
            if (sdata->playback_loop && !sdata->playback_file_path.empty()) {
                clear_decoder_process(sdata);
                if (!start_decoder_process(sdata, sdata->playback_file_path)) {
                    stop_playback(sdata);
                    queue_response(sdata, "ERR playback loop restart failed\n");
                }
            } else {
                stop_playback(sdata);
                queue_response(sdata, "OK PLAYBACK FINISHED\n");
            }
        } else if (sdata->playback_waiting_for_first_chunk) {
            send_noise_batch(sdata, target_samples);
        }
        return;
    }

    size_t send_samples = std::min(available_samples, target_samples);
    std::vector<float> mono(send_samples);

    for (size_t i = 0; i < send_samples; ++i) {
        unsigned int lo = (unsigned int)sdata->playback_pcm16_buffer[2 * i];
        unsigned int hi = (unsigned int)sdata->playback_pcm16_buffer[2 * i + 1];
        int16_t sample = (int16_t)((hi << 8) | lo);
        mono[i] = (float)sample / 32768.0f;
    }

    sdata->playback_waiting_for_first_chunk = false;
    send_playback_samples(sdata, mono.data(), send_samples);

    std::vector<unsigned char>::difference_type consumed = (std::vector<unsigned char>::difference_type)(send_samples * sizeof(int16_t));
    sdata->playback_pcm16_buffer.erase(sdata->playback_pcm16_buffer.begin(), sdata->playback_pcm16_buffer.begin() + consumed);

    if (sdata->playback_decoder_eof && sdata->playback_pcm16_buffer.empty()) {
        if (sdata->playback_loop && !sdata->playback_file_path.empty()) {
            clear_decoder_process(sdata);
            if (!start_decoder_process(sdata, sdata->playback_file_path)) {
                stop_playback(sdata);
                queue_response(sdata, "ERR playback loop restart failed\n");
            }
        } else {
            stop_playback(sdata);
            queue_response(sdata, "OK PLAYBACK FINISHED\n");
        }
    }
}

static void handle_command(file_cmd_tcp_server_data* sdata, std::string const& command_line) {
    std::string line = trim(command_line);
    if (line.empty()) {
        return;
    }
    log(LOG_INFO, "file_cmd_tcp_server: command received on %s:%s: %s\n", sdata->bind_address, sdata->bind_port, line.c_str());

    size_t sep = line.find_first_of(" \t");
    std::string command = line.substr(0, sep);
    std::string arg = sep == std::string::npos ? "" : trim(line.substr(sep + 1));
    command = to_upper_ascii(command);

    if (command == "LIST_FILES") {
        queue_response(sdata, build_list_files_response(sdata));
    } else if (command == "PLAY_FILE") {
        if (arg.empty()) {
            queue_response(sdata, "ERR missing file path\n");
            return;
        }
        std::string error_message;
        if (start_playback(sdata, arg, &error_message)) {
            sdata->playback_loop = false;
            queue_response(sdata, "OK PLAY_FILE STARTED\n");
        } else {
            queue_response(sdata, error_message);
        }
    } else if (command == "LOOP_FILE") {
        if (arg.empty()) {
            queue_response(sdata, "ERR missing file path\n");
            return;
        }
        std::string error_message;
        if (start_playback(sdata, arg, &error_message)) {
            sdata->playback_loop = true;
            queue_response(sdata, "OK LOOP_FILE STARTED\n");
        } else {
            queue_response(sdata, error_message);
        }
    } else if (command == "STOP_PLAYBACK") {
        if (sdata->playback_active) {
            stop_playback(sdata);
            queue_response(sdata, "OK PLAYBACK STOPPED\n");
        } else {
            queue_response(sdata, "OK PLAYBACK NOT_ACTIVE\n");
        }
    } else if (command == "PING") {
        queue_response(sdata, "PONG\n");
    } else if (command == "HELP") {
        queue_response(sdata, "OK COMMANDS HELP PING LIST_FILES PLAY_FILE LOOP_FILE STOP_PLAYBACK\n");
    } else {
        queue_response(sdata, "ERR unknown command\n");
    }
}

static void read_commands_if_available(file_cmd_tcp_server_data* sdata) {
    if (sdata->client_socket == -1) {
        return;
    }

    char buffer[1024];

    for (;;) {
        ssize_t count = recv(sdata->client_socket, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count > 0) {
            sdata->recv_buffer.append(buffer, (size_t)count);

            if (sdata->recv_buffer.size() > 65536) {
                queue_response(sdata, "ERR command too long\n");
                sdata->recv_buffer.clear();
            }

            size_t eol = 0;
            while ((eol = sdata->recv_buffer.find('\n')) != std::string::npos) {
                std::string line = sdata->recv_buffer.substr(0, eol);
                sdata->recv_buffer.erase(0, eol + 1);
                if (!line.empty() && line[line.size() - 1] == '\r') {
                    line.erase(line.size() - 1);
                }
                handle_command(sdata, line);
            }
            continue;
        }

        if (count == 0) {
            log(LOG_INFO, "file_cmd_tcp_server: client disconnected on %s:%s\n", sdata->bind_address, sdata->bind_port);
            close(sdata->client_socket);
            sdata->client_socket = -1;
            sdata->recv_buffer.clear();
            sdata->send_buffer.clear();
            sdata->send_offset = 0;
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        log(LOG_INFO, "file_cmd_tcp_server: recv failed on %s:%s (%s)\n", sdata->bind_address, sdata->bind_port, strerror(errno));
        close(sdata->client_socket);
        sdata->client_socket = -1;
        sdata->recv_buffer.clear();
        sdata->send_buffer.clear();
        sdata->send_offset = 0;
        return;
    }
}

static void accept_client_if_available(file_cmd_tcp_server_data* sdata) {
    if (sdata->listen_socket == -1) {
        return;
    }

    for (;;) {
        int client = accept(sdata->listen_socket, NULL, NULL);
        if (client == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            log(LOG_WARNING, "file_cmd_tcp_server: accept failed on %s:%s: %s\n", sdata->bind_address, sdata->bind_port, strerror(errno));
            return;
        }

        if (!set_nonblocking(client)) {
            log(LOG_WARNING, "file_cmd_tcp_server: failed to set nonblocking mode for client on %s:%s\n", sdata->bind_address, sdata->bind_port);
            close(client);
            continue;
        }

        if (sdata->client_socket != -1) {
            close(sdata->client_socket);
        }
        sdata->client_socket = client;
        sdata->recv_buffer.clear();
        sdata->send_buffer.clear();
        sdata->send_offset = 0;

        queue_response(sdata, "OK file_cmd_tcp_server ready\n");
        queue_response(sdata, "OK COMMANDS HELP PING LIST_FILES PLAY_FILE LOOP_FILE STOP_PLAYBACK\n");
        log(LOG_INFO, "file_cmd_tcp_server: client connected on %s:%s\n", sdata->bind_address, sdata->bind_port);
    }
}

void file_cmd_tcp_server_set_dirs(file_cmd_tcp_server_data* sdata, channel_t const* channel) {
    sdata->directories.clear();
    sdata->playback_udp_stream = NULL;
    sdata->playback_udp_stream_server = NULL;
    sdata->playback_mode = channel->mode;

    for (int i = 0; i < channel->output_count; ++i) {
        output_t const* output = channel->outputs + i;
        if (output->type == O_FILE || output->type == O_RAWFILE
#ifdef WITH_FLAC_FILE_OUTPUT
            || output->type == O_FLAC_FILE
#endif
        ) {
            file_data* fdata = (file_data*)(output->data);
            sdata->directories.push_back(fdata->basedir);
        } else if (output->type == O_UDP_STREAM && sdata->playback_udp_stream == NULL) {
            sdata->playback_udp_stream = (udp_stream_data*)output->data;
        } else if (output->type == O_UDP_STREAM_SERVER && sdata->playback_udp_stream_server == NULL) {
            sdata->playback_udp_stream_server = (udp_stream_server_data*)output->data;
        }
    }

    std::sort(sdata->directories.begin(), sdata->directories.end());
    sdata->directories.erase(std::unique(sdata->directories.begin(), sdata->directories.end()), sdata->directories.end());
}

bool file_cmd_tcp_server_init(file_cmd_tcp_server_data* sdata) {
    sdata->listen_socket = -1;
    sdata->client_socket = -1;
    sdata->recv_buffer.clear();
    sdata->send_buffer.clear();
    sdata->send_offset = 0;

    sdata->playback_active = false;
    sdata->playback_loop = false;
    sdata->playback_pipe_fd = -1;
    sdata->playback_stderr_fd = -1;
    sdata->playback_decoder_pid = -1;
    sdata->playback_decoder_eof = false;
    sdata->playback_file_path.clear();
    sdata->playback_pcm16_buffer.clear();
    sdata->playback_decoder_stderr.clear();
    sdata->playback_waiting_for_first_chunk = false;
    sdata->playback_noise_state = 0x12345678u;

    struct addrinfo hints;
    struct addrinfo* result;
    struct addrinfo* rptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int error = getaddrinfo(sdata->bind_address, sdata->bind_port, &hints, &result);
    if (error) {
        log(LOG_ERR, "file_cmd_tcp_server: could not resolve bind address %s:%s - %s\n", sdata->bind_address, sdata->bind_port, gai_strerror(error));
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
        log(LOG_ERR, "file_cmd_tcp_server: could not bind/listen on %s:%s\n", sdata->bind_address, sdata->bind_port);
        return false;
    }

    log(LOG_INFO, "file_cmd_tcp_server: listening for commands on %s:%s\n", sdata->bind_address, sdata->bind_port);
    return true;
}

void file_cmd_tcp_server_poll(file_cmd_tcp_server_data* sdata) {
    accept_client_if_available(sdata);
    read_commands_if_available(sdata);
    pump_playback(sdata);
    flush_send_buffer(sdata);
}

void file_cmd_tcp_server_shutdown(file_cmd_tcp_server_data* sdata) {
    stop_playback(sdata);

    if (sdata->client_socket != -1) {
        close(sdata->client_socket);
        sdata->client_socket = -1;
    }
    if (sdata->listen_socket != -1) {
        close(sdata->listen_socket);
        sdata->listen_socket = -1;
    }

    sdata->recv_buffer.clear();
    sdata->send_buffer.clear();
    sdata->send_offset = 0;
}
