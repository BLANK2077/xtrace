#include "session_registry.h"
#include "../protocol/protocol.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/file.h>
#include <sstream>

namespace xtrace {

SessionRegistry::SessionRegistry() {
    get_registry_path(registry_path_);
}

SessionRegistry::~SessionRegistry() {
}

bool SessionRegistry::lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

bool SessionRegistry::unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

std::string SessionRegistry::serialize(const SessionInfo& session) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "%d|%s|%s|%d|%ld|%ld|%s|%ld|%lld|%llu|%llu\n",
             session.session_id,
             session.socket_path.c_str(),
             session.design_file.c_str(),
             session.server_pid,
             session.created_at,
             session.last_active,
             session.dbdir_path.c_str(),
             session.dbdir_mtime,
             session.dbdir_size,
             session.dbdir_dev,
             session.dbdir_inode);
    return std::string(buf);
}

bool SessionRegistry::parse_line(const char* line, SessionInfo& session) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, '|')) {
        if (!field.empty() && field.back() == '\n') field.pop_back();
        if (!field.empty() && field.back() == '\r') field.pop_back();
        fields.push_back(field);
    }

    if (fields.size() != 5 && fields.size() != 6 && fields.size() != 11) {
        return false;
    }

    char* end = nullptr;
    session.session_id = strtol(fields[0].c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    session.socket_path = fields[1];
    session.design_file = fields[2];
    session.server_pid = strtol(fields[3].c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    session.created_at = strtol(fields[4].c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    session.last_active = session.created_at;

    if (fields.size() >= 6) {
        session.last_active = strtol(fields[5].c_str(), &end, 10);
        if (!end || *end != '\0') return false;
    }

    if (fields.size() == 11) {
        session.dbdir_path = fields[6];
        session.dbdir_mtime = strtol(fields[7].c_str(), &end, 10);
        if (!end || *end != '\0') return false;
        session.dbdir_size = strtoll(fields[8].c_str(), &end, 10);
        if (!end || *end != '\0') return false;
        session.dbdir_dev = strtoull(fields[9].c_str(), &end, 10);
        if (!end || *end != '\0') return false;
        session.dbdir_inode = strtoull(fields[10].c_str(), &end, 10);
        if (!end || *end != '\0') return false;
    }

    return session.session_id > 0;
}

bool SessionRegistry::load_all(std::vector<SessionInfo>& sessions) {
    sessions.clear();

    int fd = open(registry_path_, O_RDONLY | O_CREAT, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    FILE* fp = fdopen(fd, "r");
    if (!fp) {
        unlock_file(fd);
        close(fd);
        return false;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        SessionInfo session;
        if (parse_line(line, session)) {
            sessions.push_back(session);
        }
    }

    fclose(fp);  // This also closes fd
    return true;
}

bool SessionRegistry::add(const SessionInfo& session) {
    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    std::string data = serialize(session);
    ssize_t written = write(fd, data.c_str(), data.length());

    unlock_file(fd);
    close(fd);

    return written == (ssize_t)data.length();
}

bool SessionRegistry::upsert(const SessionInfo& session) {
    std::vector<SessionInfo> sessions;
    load_all(sessions);

    bool replaced = false;
    for (auto& s : sessions) {
        if (s.session_id == session.session_id) {
            s = session;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        sessions.push_back(session);
    }

    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    bool ok = true;
    for (const auto& s : sessions) {
        std::string data = serialize(s);
        if (write(fd, data.c_str(), data.length()) != (ssize_t)data.length()) {
            ok = false;
        }
    }

    unlock_file(fd);
    close(fd);
    return ok;
}

bool SessionRegistry::touch(int session_id, time_t last_active) {
    SessionInfo session;
    if (!get(session_id, session)) return false;
    session.last_active = last_active;
    return upsert(session);
}

bool SessionRegistry::remove(int session_id) {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    // Rewrite registry without the removed session
    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    bool found = false;
    for (const auto& session : sessions) {
        if (session.session_id == session_id) {
            found = true;
            continue;
        }
        std::string data = serialize(session);
        write(fd, data.c_str(), data.length());
    }

    unlock_file(fd);
    close(fd);

    return found;
}

bool SessionRegistry::get(int session_id, SessionInfo& session) {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    for (const auto& s : sessions) {
        if (s.session_id == session_id) {
            session = s;
            return true;
        }
    }
    return false;
}

bool SessionRegistry::get_latest(SessionInfo& session) {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions) || sessions.empty()) return false;

    // Find session with highest ID
    int max_id = -1;
    size_t max_idx = 0;
    for (size_t i = 0; i < sessions.size(); ++i) {
        if (sessions[i].session_id > max_id) {
            max_id = sessions[i].session_id;
            max_idx = i;
        }
    }

    session = sessions[max_idx];
    return true;
}

int SessionRegistry::get_next_id() {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return 1;

    int max_id = 0;
    for (const auto& s : sessions) {
        if (s.session_id > max_id) {
            max_id = s.session_id;
        }
    }
    return max_id + 1;
}

bool SessionRegistry::cleanup_stale() {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    std::vector<SessionInfo> valid_sessions;

    for (const auto& session : sessions) {
        // Check if process is still alive
        bool is_alive = (kill(session.server_pid, 0) == 0);

        // Also check if socket file exists
        bool socket_exists = (access(session.socket_path.c_str(), F_OK) == 0);

        if (is_alive && socket_exists) {
            valid_sessions.push_back(session);
        } else {
            // Clean up stale socket file
            if (socket_exists) {
                unlink(session.socket_path.c_str());
            }
        }
    }

    // Rewrite registry with only valid sessions
    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    for (const auto& session : valid_sessions) {
        std::string data = serialize(session);
        write(fd, data.c_str(), data.length());
    }

    unlock_file(fd);
    close(fd);

    return true;
}

bool SessionRegistry::clear_all() {
    // Remove all socket files first
    std::vector<SessionInfo> sessions;
    if (load_all(sessions)) {
        for (const auto& session : sessions) {
            unlink(session.socket_path.c_str());
        }
    }

    // Delete registry file
    unlink(registry_path_);
    return true;
}

} // namespace xtrace
