#include "session_manager.h"
#include "../common/xtrace_paths.h"
#include "../protocol/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <cstdarg>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>

namespace xtrace {

namespace {

int connect_socket_path(const char* sock_path) {
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

bool ping_socket_path(const char* sock_path) {
    int fd = connect_socket_path(sock_path);
    if (fd < 0) return false;

    const char* ping_msg = CMD_PING "\n";
    if (write(fd, ping_msg, strlen(ping_msg)) < 0) {
        close(fd);
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[64];
    bool got_pong = false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        got_pong = strstr(buf, "PONG") != nullptr;
    }

    close(fd);
    return got_pong;
}

bool protocol_version_matches(const char* sock_path) {
    int fd = connect_socket_path(sock_path);
    if (fd < 0) return false;

    const char* version_msg = CMD_VERSION "\n";
    if (write(fd, version_msg, strlen(version_msg)) < 0) {
        close(fd);
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[128];
    bool matched = false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        matched = strstr(buf, PROTOCOL_VERSION) != nullptr;
    }

    close(fd);
    return matched;
}

}  // namespace

bool xtrace_debug_enabled() {
    const char* env = getenv("XTRACE_DEBUG");
    return env && env[0] != '\0' && strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 && strcasecmp(env, "off") != 0;
}

void SessionManager::debug_log(const char* fmt, ...) const {
    if (!xtrace_debug_enabled()) return;
    fprintf(stderr, "[xtrace-debug] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

const char* session_health_status_name(SessionHealthStatus status) {
    switch (status) {
        case SessionHealthStatus::Healthy:
            return "healthy";
        case SessionHealthStatus::RegistryMissing:
            return "registry_missing";
        case SessionHealthStatus::ProcessExited:
            return "process_exited";
        case SessionHealthStatus::SocketMissing:
            return "socket_missing";
        case SessionHealthStatus::ConnectFailed:
            return "connect_failed";
        case SessionHealthStatus::PingFailed:
            return "ping_failed";
        case SessionHealthStatus::DbdirMissing:
            return "dbdir_missing";
        case SessionHealthStatus::DbdirChanged:
            return "dbdir_changed";
    }
    return "unknown";
}

SessionManager::SessionManager() : registry_(new SessionRegistry()) {
}

SessionManager::~SessionManager() {
}

pid_t SessionManager::spawn_server(const std::string& session_id, const std::vector<std::string>& args) {
    // Get path to current executable
    char self_path[1024] = {};
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len < 0) {
        return -1;
    }

    // Build server argv: [exe, "--server", session_id, ...design_args...]
    std::vector<char*> argv;
    argv.push_back(self_path);
    argv.push_back((char*)"--server");

    std::string session_id_arg = session_id;
    argv.push_back(const_cast<char*>(session_id_arg.c_str()));

    std::vector<std::string> arg_storage = args;  // Keep strings alive
    for (const auto& arg : arg_storage) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        // Detach the session server from the short-lived CLI process so it
        // survives after `xtrace open ...` exits.
        if (setsid() < 0) {
            _exit(1);
        }
        signal(SIGHUP, SIG_IGN);

        // Child process - exec server
        execv(self_path, argv.data());
        perror("execv");
        _exit(1);
    }

    return pid;
}

std::string SessionManager::canonicalize_dbdir_path(const std::string& dbdir_path) const {
    char resolved[PATH_MAX];
    if (realpath(dbdir_path.c_str(), resolved)) {
        return std::string(resolved);
    }
    return dbdir_path;
}

bool SessionManager::populate_dbdir_metadata(const std::string& dbdir_path, SessionInfo& session) const {
    struct stat st;
    if (stat(dbdir_path.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    session.dbdir_path = dbdir_path;
    session.design_file = dbdir_path;
    session.dbdir_mtime = static_cast<long>(st.st_mtime);
    session.dbdir_size = static_cast<long long>(st.st_size);
    session.dbdir_dev = static_cast<unsigned long long>(st.st_dev);
    session.dbdir_inode = static_cast<unsigned long long>(st.st_ino);
    return true;
}

bool SessionManager::current_dbdir_metadata(const SessionInfo& session, SessionInfo& current) const {
    if (session.dbdir_path.empty()) return false;
    current = session;
    return populate_dbdir_metadata(session.dbdir_path, current);
}

bool SessionManager::dbdir_metadata_matches(const SessionInfo& expected, const SessionInfo& current) const {
    return expected.dbdir_mtime == current.dbdir_mtime &&
           expected.dbdir_size == current.dbdir_size &&
           expected.dbdir_dev == current.dbdir_dev &&
           expected.dbdir_inode == current.dbdir_inode;
}

bool SessionManager::parse_open_args(const std::vector<std::string>& design_args,
                                     std::string& canonical_dbdir,
                                     std::vector<std::string>& canonical_args) const {
    if (design_args.size() < 2 || design_args[0] != "-dbdir") {
        return false;
    }

    std::string dbdir = design_args[1];
    while (dbdir.size() > 1 && dbdir.back() == '/') {
        dbdir.pop_back();
    }

    const std::string suffix = ".daidir";
    if (dbdir.size() < suffix.size() ||
        dbdir.compare(dbdir.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }

    canonical_dbdir = canonicalize_dbdir_path(dbdir);
    SessionInfo metadata;
    if (!populate_dbdir_metadata(canonical_dbdir, metadata)) {
        return false;
    }

    canonical_args = design_args;
    canonical_args[1] = canonical_dbdir;
    return true;
}

WaitForServerResult SessionManager::wait_for_server(const std::string& session_id, pid_t pid) {
    WaitForServerResult result;
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);

    int timeout_sec = 60;
    const char* env = getenv("XTRACE_SESSION_START_TIMEOUT_SEC");
    if (env && *env) {
        int v = atoi(env);
        if (v > 0) timeout_sec = v;
    }
    int loops = timeout_sec * 10;
    if (loops <= 0) loops = 600;
    debug_log("wait_for_server: session=%s pid=%d socket=%s timeout_sec=%d",
              session_id.c_str(), pid, sock_path, timeout_sec);

    for (int i = 0; i < loops; ++i) {
        usleep(100000);  // 100ms
        result.elapsed_ms = (i + 1) * 100L;

        result.socket_exists = access(sock_path, F_OK) == 0;
        if (result.socket_exists) {
            int fd = connect_socket_path(sock_path);
            if (fd >= 0) {
                result.connect_ok = true;
                close(fd);
                result.ping_ok = ping_socket_path(sock_path);
                if (result.ping_ok) {
                    result.ok = true;
                    result.reason = "ready";
                    debug_log("wait_for_server: socket_exists=1 connect_ok=1 ping_ok=1 elapsed_ms=%ld",
                              result.elapsed_ms);
                    return result;
                }
            }
        }

        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            result.child_exited = true;
            result.child_status = status;
            result.reason = "child_exited";
            debug_log("wait_for_server: child_exited status=%d elapsed_ms=%ld socket_exists=%d connect_ok=%d ping_ok=%d",
                      status, result.elapsed_ms, result.socket_exists ? 1 : 0,
                      result.connect_ok ? 1 : 0, result.ping_ok ? 1 : 0);
            return result;
        }
    }

    result.reason = result.socket_exists ? (result.connect_ok ? "ping_failed" : "socket_connect_failed")
                                         : "timeout_waiting_socket";
    debug_log("wait_for_server: timeout reason=%s elapsed_ms=%ld socket_exists=%d connect_ok=%d ping_ok=%d",
              result.reason.c_str(), result.elapsed_ms, result.socket_exists ? 1 : 0,
              result.connect_ok ? 1 : 0, result.ping_ok ? 1 : 0);
    return result;
}

SessionEnsureResult SessionManager::ensure_session(const std::vector<std::string>& design_args, const std::string& session_name) {
    SessionEnsureResult result;

    std::string canonical_dbdir;
    std::vector<std::string> canonical_args;
    if (!parse_open_args(design_args, canonical_dbdir, canonical_args)) {
        debug_log("ensure_session: reason=invalid_args");
        result.status = "invalid_args";
        result.message = "Usage: open -dbdir <simv.daidir> [args...]";
        return result;
    }
    debug_log("ensure_session: canonical_dbdir=%s", canonical_dbdir.c_str());

    if (!SessionRegistry::is_valid_session_name(session_name)) {
        result.status = "invalid_session_id";
        result.message = "Session name is required and must be 1-256 chars using [A-Za-z0-9_.-]";
        debug_log("ensure_session: reason=invalid_session_id name=%s", session_name.c_str());
        return result;
    }

    // Clean up stale sessions first
    debug_log("ensure_session: cleanup_stale_begin");
    cleanup();
    debug_log("ensure_session: cleanup_stale_done");

    if (registry_->exists(session_name)) {
        result.status = "session_id_exists";
        result.message = "Session id already exists: " + session_name;
        debug_log("ensure_session: reason=session_id_exists name=%s", session_name.c_str());
        return result;
    }

    std::string session_id = session_name;
    if (!xtrace_ensure_session_dir(session_id)) {
        result.status = "session_dir_failed";
        result.message = "Failed to create session directory";
        debug_log("ensure_session: reason=session_dir_failed session=%s", session_id.c_str());
        return result;
    }
    debug_log("ensure_session: session_id=%s", session_id.c_str());

    // Spawn server process
    pid_t pid = spawn_server(session_id, canonical_args);
    if (pid < 0) {
        result.status = "spawn_failed";
        result.message = "Failed to spawn xtrace server";
        xtrace_remove_session_dir(session_id);
        debug_log("ensure_session: reason=spawn_failed session=%s", session_id.c_str());
        return result;
    }
    debug_log("ensure_session: spawned_server session=%s pid=%d", session_id.c_str(), pid);

    // Get socket path
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);
    char dbg_path[SOCK_PATH_LEN];
    get_debug_log_path(dbg_path, session_id);
    debug_log("ensure_session: socket_path=%s debug_log=%s", sock_path, dbg_path);

    WaitForServerResult wait = wait_for_server(session_id, pid);
    if (!wait.ok) {
        // Kill the server process if it didn't start properly
        kill(pid, SIGTERM);
        unlink(sock_path);
        xtrace_remove_session_dir(session_id);
        result.status = "startup_failed";
        result.message = "Server did not become ready: " + wait.reason;
        debug_log("ensure_session: reason=%s elapsed_ms=%ld child_exited=%d child_status=%d socket_exists=%d connect_ok=%d ping_ok=%d",
                  wait.reason.c_str(), wait.elapsed_ms, wait.child_exited ? 1 : 0,
                  wait.child_status, wait.socket_exists ? 1 : 0,
                  wait.connect_ok ? 1 : 0, wait.ping_ok ? 1 : 0);
        return result;
    }

    // Create session info
    SessionInfo session;
    session.session_id = session_id;
    session.socket_path = sock_path;
    session.server_pid = pid;
    session.created_at = time(nullptr);
    session.last_active = session.created_at;
    populate_dbdir_metadata(canonical_dbdir, session);

    // Add to registry
    if (!registry_->add(session)) {
        kill(pid, SIGTERM);
        unlink(sock_path);
        xtrace_remove_session_dir(session_id);
        result.status = "registry_failed";
        result.message = "Failed to update session registry";
        debug_log("ensure_session: reason=registry_failed session=%s", session_id.c_str());
        return result;
    }

    result.ok = true;
    result.reused = false;
    result.session_id = session_id;
    result.status = "healthy";
    result.message = "Created healthy session";
    result.info = session;
    debug_log("ensure_session: success session=%s pid=%d socket=%s", session_id.c_str(), pid, sock_path);
    return result;
}

SessionEnsureResult SessionManager::create_session(const std::vector<std::string>& design_args, const std::string& session_name) {
    return ensure_session(design_args, session_name);
}

bool SessionManager::kill_session(const std::string& session_id) {
    SessionInfo session;
    if (!registry_->get(session_id, session)) {
        debug_log("kill_session: registry_missing session=%s", session_id.c_str());
        return false;
    }
    debug_log("kill_session: begin session=%s pid=%d socket=%s",
              session.session_id.c_str(), session.server_pid, session.socket_path.c_str());

    int fd = connect_socket_path(session.socket_path.c_str());
    if (fd < 0) {
        if (kill(session.server_pid, 0) == 0) {
            kill(session.server_pid, SIGTERM);
        }
        registry_->remove(session_id);
        return true;
    }

    // Send QUIT command
    const char* quit_msg = CMD_QUIT "\n";
    write(fd, quit_msg, strlen(quit_msg));

    // Wait for response or timeout (short, non-blocking to user)
    char buf[64];
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;  // 500ms
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (read(fd, buf, sizeof(buf)) > 0) {
        // Drain response
    }

    close(fd);

    // Give server a brief moment to exit gracefully
    int status;
    usleep(300000);  // 300ms
    waitpid(session.server_pid, &status, WNOHANG);

    // Force kill if still alive
    if (kill(session.server_pid, 0) == 0) {
        kill(session.server_pid, SIGTERM);
    }

    // Remove from registry
    registry_->remove(session_id);

    return true;
}

bool SessionManager::kill_all_sessions() {
    std::vector<SessionInfo> sessions = list_sessions();
    for (const auto& session : sessions) {
        kill_session(session.session_id);
    }
    registry_->clear_all();
    return true;
}

bool SessionManager::get_session(const std::string& session_id, SessionInfo& info) {
    return registry_->get(session_id, info);
}

bool SessionManager::get_latest_session(SessionInfo& info) {
    return registry_->get_latest(info);
}

bool SessionManager::touch_session(const std::string& session_id) {
    return registry_->touch(session_id, time(nullptr));
}

std::vector<SessionInfo> SessionManager::list_sessions() {
    cleanup();
    std::vector<SessionInfo> sessions;
    registry_->load_all(sessions);
    return sessions;
}

SessionHealth SessionManager::diagnose_session(const std::string& session_id) {
    SessionHealth health;
    health.session_id = session_id;

    SessionInfo session;
    if (!registry_->get(session_id, session)) {
        health.status = SessionHealthStatus::RegistryMissing;
        health.message = "Session is not present in the registry";
        return health;
    }

    health.info = session;

    SessionInfo current;
    if (!current_dbdir_metadata(session, current)) {
        health.status = SessionHealthStatus::DbdirMissing;
        health.message = "Daidir path is missing, is not a directory, or lacks metadata";
        return health;
    }
    if (!dbdir_metadata_matches(session, current)) {
        health.status = SessionHealthStatus::DbdirChanged;
        health.message = "Daidir metadata changed since session was opened";
        return health;
    }

    if (kill(session.server_pid, 0) != 0) {
        health.status = SessionHealthStatus::ProcessExited;
        health.message = "Server process is not running";
        return health;
    }

    if (access(session.socket_path.c_str(), F_OK) != 0) {
        health.status = SessionHealthStatus::SocketMissing;
        health.message = "Server socket file is missing";
        return health;
    }

    int fd = connect_socket_path(session.socket_path.c_str());
    if (fd < 0) {
        health.status = SessionHealthStatus::ConnectFailed;
        health.message = "Server socket exists but cannot be connected";
        return health;
    }
    close(fd);

    if (!ping_socket_path(session.socket_path.c_str())) {
        health.status = SessionHealthStatus::PingFailed;
        health.message = "Server did not respond to PING";
        return health;
    }

    health.healthy = true;
    health.status = SessionHealthStatus::Healthy;
    health.message = "Session is healthy";
    return health;
}

bool SessionManager::is_session_alive(const std::string& session_id) {
    return diagnose_session(session_id).healthy;
}

std::string SessionManager::get_socket_path(const std::string& session_id) {
    char path[SOCK_PATH_LEN];
    get_sock_path(path, session_id);
    return std::string(path);
}

void SessionManager::cleanup() {
    debug_log("cleanup: begin");
    registry_->cleanup_stale();
    debug_log("cleanup: done");
}

} // namespace xtrace
