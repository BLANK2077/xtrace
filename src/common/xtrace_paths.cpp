#include "xtrace_paths.h"

#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xtrace {

namespace {

std::string home_dir() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home);
}

bool ensure_dir(const std::string& path) {
    if (mkdir(path.c_str(), 0700) == 0) return true;
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool remove_file_if_exists(const std::string& path) {
    if (unlink(path.c_str()) == 0) return true;
    return access(path.c_str(), F_OK) != 0;
}

} // namespace

std::string xtrace_home_dir() {
    return home_dir() + "/.xtrace";
}

std::string xtrace_sessions_dir() {
    return xtrace_home_dir() + "/sessions";
}

std::string session_dir_name(const std::string& session_id) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : session_id) {
        h ^= static_cast<unsigned long long>(c);
        h *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << "s_" << std::hex << h;
    return oss.str();
}

std::string xtrace_session_dir(const std::string& session_id) {
    return xtrace_sessions_dir() + "/" + session_dir_name(session_id);
}

std::string xtrace_registry_path() {
    return xtrace_home_dir() + "/registry.json";
}

std::string xtrace_registry_lock_path() {
    return xtrace_home_dir() + "/registry.lock";
}

std::string xtrace_session_json_path(const std::string& session_id) {
    return xtrace_session_dir(session_id) + "/session.json";
}

std::string xtrace_socket_path(const std::string& session_id) {
    return xtrace_session_dir(session_id) + "/socket";
}

std::string xtrace_debug_log_path(const std::string& session_id) {
    return xtrace_session_dir(session_id) + "/debug.log";
}

std::string xtrace_legacy_registry_path() {
    return home_dir() + "/.xtrace.registry";
}

bool xtrace_ensure_home() {
    bool ok = ensure_dir(xtrace_home_dir()) && ensure_dir(xtrace_sessions_dir());
    if (ok) {
        int fd = open(xtrace_registry_lock_path().c_str(), O_RDWR | O_CREAT, 0600);
        if (fd >= 0) close(fd);
    }
    return ok;
}

bool xtrace_ensure_session_dir(const std::string& session_id) {
    return xtrace_ensure_home() && ensure_dir(xtrace_session_dir(session_id));
}

bool xtrace_remove_session_dir(const std::string& session_id) {
    std::string dir = xtrace_session_dir(session_id);
    remove_file_if_exists(dir + "/session.json");
    remove_file_if_exists(dir + "/socket");
    remove_file_if_exists(dir + "/debug.log");
    if (rmdir(dir.c_str()) == 0) return true;
    return access(dir.c_str(), F_OK) != 0;
}

} // namespace xtrace
