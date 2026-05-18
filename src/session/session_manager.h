#pragma once

#include "session_registry.h"
#include <memory>
#include <string>

namespace xtrace {

enum class SessionHealthStatus {
    Healthy,
    RegistryMissing,
    ProcessExited,
    SocketMissing,
    ConnectFailed,
    PingFailed,
    DbdirMissing,
    DbdirChanged
};

struct SessionHealth {
    std::string session_id;
    bool healthy = false;
    SessionHealthStatus status = SessionHealthStatus::RegistryMissing;
    std::string message;
    SessionInfo info;
};

struct SessionEnsureResult {
    std::string session_id;
    bool ok = false;
    bool reused = false;
    std::string status;
    std::string message;
    SessionInfo info;
};

struct WaitForServerResult {
    bool ok = false;
    std::string reason;
    long elapsed_ms = 0;
    bool child_exited = false;
    int child_status = 0;
    bool socket_exists = false;
    bool connect_ok = false;
    bool ping_ok = false;
};

const char* session_health_status_name(SessionHealthStatus status);

bool xtrace_debug_enabled();

// Session manager - high-level session lifecycle management
class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    // Create a new session, returns session ID (0 on failure)
    // This spawns the server process
    SessionEnsureResult create_session(const std::vector<std::string>& design_args, const std::string& session_name);

    // Ensure a healthy session exists for a dbdir argument list.
    SessionEnsureResult ensure_session(const std::vector<std::string>& design_args, const std::string& session_name);

    // Kill a specific session (calls npi_end() in server)
    bool kill_session(const std::string& session_id);

    // Kill all sessions
    bool kill_all_sessions();

    // Get session info by ID
    bool get_session(const std::string& session_id, SessionInfo& info);

    // Get the latest (most recent) session
    bool get_latest_session(SessionInfo& info);

    // Update activity timestamp
    bool touch_session(const std::string& session_id);

    // List all active sessions
    std::vector<SessionInfo> list_sessions();

    // Diagnose a session without mutating the registry
    SessionHealth diagnose_session(const std::string& session_id);

    // Check if a session is alive
    bool is_session_alive(const std::string& session_id);

    // Get socket path for a session
    std::string get_socket_path(const std::string& session_id);

    // Clean up stale sessions
    void cleanup();

private:
    std::unique_ptr<SessionRegistry> registry_;

    // Fork and exec server process
    pid_t spawn_server(const std::string& session_id, const std::vector<std::string>& args);

    // Wait until the server responds to PING
    WaitForServerResult wait_for_server(const std::string& session_id, pid_t pid);
    void debug_log(const char* fmt, ...) const;

    bool parse_open_args(const std::vector<std::string>& design_args,
                         std::string& canonical_dbdir,
                         std::vector<std::string>& canonical_args) const;
    bool populate_dbdir_metadata(const std::string& dbdir_path, SessionInfo& session) const;
    bool current_dbdir_metadata(const SessionInfo& session, SessionInfo& current) const;
    bool dbdir_metadata_matches(const SessionInfo& expected, const SessionInfo& current) const;
    std::string canonicalize_dbdir_path(const std::string& dbdir_path) const;
};

} // namespace xtrace
