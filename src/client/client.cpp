#include "client.h"
#include "../protocol/protocol.h"
#include "../session/session_manager.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

namespace xtrace {

using json = nlohmann::json;

static void print_json_error(const char* command_name,
                             int session_id,
                             const char* status,
                             const std::string& message) {
    json payload = {
        {"ok", false},
        {"command", command_name ? command_name : ""},
        {"session_id", session_id},
        {"status", status ? status : "error"},
        {"message", message}
    };
    fprintf(stderr, "%s\n", payload.dump(2).c_str());
}

int session_connect(int session_id) {
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool send_command_and_print(int session_id, const char* cmd) {
    return send_command_and_print_ex(session_id, cmd, false, "");
}

bool send_command_and_print_ex(int session_id, const char* cmd, bool json_errors, const char* command_name) {
    SessionManager manager;
    int fd = session_connect(session_id);
    if (fd < 0) {
        SessionHealth health = manager.diagnose_session(session_id);
        const char* status = session_health_status_name(health.status);
        if (json_errors) {
            print_json_error(command_name, session_id, status, health.message);
        } else {
            fprintf(stderr, "Error: Session %d unavailable: %s (status=%s)\n",
                    session_id,
                    health.message.c_str(),
                    status);
        }
        return false;
    }

    // Send command
    std::string msg = std::string(cmd) + "\n";
    if (write(fd, msg.c_str(), msg.length()) < 0) {
        close(fd);
        return false;
    }

    // Read response until END_MARKER
    std::string buf;
    const std::string end_marker(END_MARKER);
    char tmp[4096];

    bool saw_end_marker = false;
    bool server_error = false;
    while (true) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, n);

        size_t pos = buf.find(end_marker);
        if (pos != std::string::npos) {
            std::string payload = buf.substr(0, pos);
            server_error = payload.compare(0, strlen(ERROR_PREFIX), ERROR_PREFIX) == 0;
            if (server_error && json_errors) {
                std::string message = payload.substr(strlen(ERROR_PREFIX));
                print_json_error(command_name, session_id, "server_error", message);
            } else {
                FILE* stream = server_error ? stderr : stdout;
                fwrite(payload.c_str(), 1, payload.size(), stream);
            }
            saw_end_marker = true;
            break;
        }

        // Flush safe prefix if buffer grows large
        if (buf.size() > sizeof(tmp) + end_marker.size()) {
            size_t safe = buf.size() - end_marker.size();
            fwrite(buf.c_str(), 1, safe, stdout);
            buf.erase(0, safe);
        }
    }
    fflush(stdout);
    fflush(stderr);
    close(fd);
    if (saw_end_marker && !server_error) {
        manager.touch_session(session_id);
    }
    return saw_end_marker && !server_error;
}

bool send_command_capture(int session_id,
                          const char* cmd,
                          std::string& payload,
                          std::string& status,
                          std::string& message) {
    SessionManager manager;
    int fd = session_connect(session_id);
    if (fd < 0) {
        SessionHealth health = manager.diagnose_session(session_id);
        status = session_health_status_name(health.status);
        message = health.message;
        return false;
    }

    std::string msg = std::string(cmd) + "\n";
    if (write(fd, msg.c_str(), msg.length()) < 0) {
        close(fd);
        status = "write_failed";
        message = "Failed to write command to session";
        return false;
    }

    std::string buf;
    const std::string end_marker(END_MARKER);
    char tmp[4096];
    bool saw_end_marker = false;
    while (true) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t pos = buf.find(end_marker);
        if (pos != std::string::npos) {
            payload = buf.substr(0, pos);
            saw_end_marker = true;
            break;
        }
    }
    close(fd);

    if (!saw_end_marker) {
        status = "read_failed";
        message = "Failed to read complete response from session";
        return false;
    }
    if (payload.compare(0, strlen(ERROR_PREFIX), ERROR_PREFIX) == 0) {
        status = "server_error";
        message = payload.substr(strlen(ERROR_PREFIX));
        return false;
    }

    manager.touch_session(session_id);
    status = "ok";
    message.clear();
    return true;
}

bool session_ping(int session_id) {
    int fd = session_connect(session_id);
    if (fd < 0) return false;

    const char* ping = CMD_PING "\n";
    if (write(fd, ping, strlen(ping)) < 0) {
        close(fd);
        return false;
    }

    // Wait for PONG with timeout
    char buf[64];
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool got_pong = false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (strstr(buf, "PONG")) {
            got_pong = true;
        }
    }

    close(fd);
    return got_pong;
}

} // namespace xtrace
