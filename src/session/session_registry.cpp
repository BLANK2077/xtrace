#include "session_registry.h"
#include "../common/xtrace_paths.h"
#include "json.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/file.h>
#include <sstream>

namespace xtrace {

using json = nlohmann::json;

SessionRegistry::SessionRegistry() {
    xtrace_ensure_home();
    registry_path_ = xtrace_registry_path();
}

SessionRegistry::~SessionRegistry() {
}

bool SessionRegistry::lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

bool SessionRegistry::unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

static json session_to_json(const SessionInfo& session) {
    return json{
        {"session_id", session.session_id},
        {"socket_path", session.socket_path},
        {"design_file", session.design_file},
        {"dbdir_path", session.dbdir_path},
        {"server_pid", session.server_pid},
        {"created_at", static_cast<long long>(session.created_at)},
        {"last_active", static_cast<long long>(session.last_active)},
        {"dbdir_mtime", session.dbdir_mtime},
        {"dbdir_size", session.dbdir_size},
        {"dbdir_dev", session.dbdir_dev},
        {"dbdir_inode", session.dbdir_inode}
    };
}

static bool json_to_session(const json& j, SessionInfo& session) {
    if (!j.is_object()) return false;
    session.session_id = j.value("session_id", 0);
    session.socket_path = j.value("socket_path", "");
    session.design_file = j.value("design_file", "");
    session.dbdir_path = j.value("dbdir_path", "");
    session.server_pid = static_cast<pid_t>(j.value("server_pid", 0));
    session.created_at = static_cast<time_t>(j.value("created_at", 0LL));
    session.last_active = static_cast<time_t>(j.value("last_active", 0LL));
    session.dbdir_mtime = j.value("dbdir_mtime", 0L);
    session.dbdir_size = j.value("dbdir_size", 0LL);
    session.dbdir_dev = j.value("dbdir_dev", 0ULL);
    session.dbdir_inode = j.value("dbdir_inode", 0ULL);
    if (session.socket_path.empty() && session.session_id > 0) {
        session.socket_path = xtrace_socket_path(session.session_id);
    }
    if (session.dbdir_path.empty()) session.dbdir_path = session.design_file;
    if (session.design_file.empty()) session.design_file = session.dbdir_path;
    return session.session_id > 0 && !session.dbdir_path.empty();
}

bool SessionRegistry::parse_legacy_line(const char* line, SessionInfo& session) {
    std::vector<std::string> fields;
    std::stringstream ss(line ? line : "");
    std::string field;
    while (std::getline(ss, field, '|')) {
        if (!field.empty() && field.back() == '\n') field.pop_back();
        if (!field.empty() && field.back() == '\r') field.pop_back();
        fields.push_back(field);
    }

    if (fields.size() != 5 && fields.size() != 6 && fields.size() != 11) return false;

    char* end = nullptr;
    session.session_id = strtol(fields[0].c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    session.socket_path = xtrace_socket_path(session.session_id);
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
    } else {
        session.dbdir_path = session.design_file;
    }

    if (session.design_file.empty()) session.design_file = session.dbdir_path;
    return session.session_id > 0;
}

bool SessionRegistry::load_legacy(std::vector<SessionInfo>& sessions) {
    sessions.clear();
    int fd = open(xtrace_legacy_registry_path().c_str(), O_RDONLY);
    if (fd < 0) return false;
    FILE* fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return false;
    }

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        SessionInfo session;
        if (parse_legacy_line(line, session)) sessions.push_back(session);
    }
    fclose(fp);
    return true;
}

bool SessionRegistry::load_all(std::vector<SessionInfo>& sessions) {
    sessions.clear();
    xtrace_ensure_home();

    int fd = open(registry_path_.c_str(), O_RDONLY);
    if (fd < 0) {
        if (load_legacy(sessions)) save_all(sessions);
        return true;
    }

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

    std::string text;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) text += buf;
    fclose(fp);

    if (text.empty()) return true;
    try {
        json root = json::parse(text);
        if (!root.is_object() || !root.value("sessions", json::array()).is_array()) return true;
        for (const auto& item : root["sessions"]) {
            SessionInfo session;
            if (json_to_session(item, session)) sessions.push_back(session);
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool SessionRegistry::write_session_file(const SessionInfo& session) {
    if (!xtrace_ensure_session_dir(session.session_id)) return false;
    int fd = open(xtrace_session_json_path(session.session_id).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    json root = {{"version", 1}, {"session", session_to_json(session)}};
    std::string data = root.dump(2) + "\n";
    bool ok = write(fd, data.c_str(), data.size()) == static_cast<ssize_t>(data.size());
    close(fd);
    return ok;
}

bool SessionRegistry::save_all(const std::vector<SessionInfo>& sessions) {
    xtrace_ensure_home();
    int fd = open(registry_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    json root;
    root["version"] = 1;
    root["sessions"] = json::array();
    for (const auto& session : sessions) root["sessions"].push_back(session_to_json(session));
    std::string data = root.dump(2) + "\n";
    bool ok = write(fd, data.c_str(), data.size()) == static_cast<ssize_t>(data.size());
    unlock_file(fd);
    close(fd);
    if (!ok) return false;
    for (const auto& session : sessions) write_session_file(session);
    return true;
}

bool SessionRegistry::add(const SessionInfo& session) {
    std::vector<SessionInfo> sessions;
    load_all(sessions);
    sessions.push_back(session);
    return save_all(sessions);
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
    if (!replaced) sessions.push_back(session);
    return save_all(sessions);
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
    std::vector<SessionInfo> kept;
    bool found = false;
    for (const auto& session : sessions) {
        if (session.session_id == session_id) {
            found = true;
            continue;
        }
        kept.push_back(session);
    }
    if (!found) return false;
    bool ok = save_all(kept);
    xtrace_remove_session_dir(session_id);
    return ok;
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
        if (s.session_id > max_id) max_id = s.session_id;
    }
    return max_id + 1;
}

bool SessionRegistry::cleanup_stale() {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;
    std::vector<SessionInfo> valid_sessions;
    for (const auto& session : sessions) {
        bool is_alive = (kill(session.server_pid, 0) == 0);
        bool socket_exists = (access(session.socket_path.c_str(), F_OK) == 0);
        if (is_alive && socket_exists) {
            valid_sessions.push_back(session);
        } else {
            xtrace_remove_session_dir(session.session_id);
        }
    }
    return save_all(valid_sessions);
}

bool SessionRegistry::clear_all() {
    std::vector<SessionInfo> sessions;
    if (load_all(sessions)) {
        for (const auto& session : sessions) xtrace_remove_session_dir(session.session_id);
    }
    std::vector<SessionInfo> empty;
    return save_all(empty);
}

} // namespace xtrace
