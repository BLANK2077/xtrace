#pragma once

#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>

namespace xtrace {

// Session information structure
struct SessionInfo {
    int session_id;             // Unique session ID
    std::string socket_path;    // Unix domain socket path
    std::string design_file;    // Design file loaded
    std::string dbdir_path;     // Canonical VCS daidir path
    pid_t server_pid;           // Server process ID
    time_t created_at;          // Creation timestamp
    time_t last_active;         // Last command activity timestamp
    long dbdir_mtime;           // daidir modification timestamp
    long long dbdir_size;       // daidir size in bytes
    unsigned long long dbdir_dev;    // daidir device ID
    unsigned long long dbdir_inode;  // daidir inode

    SessionInfo()
        : session_id(0),
          server_pid(0),
          created_at(0),
          last_active(0),
          dbdir_mtime(0),
          dbdir_size(0),
          dbdir_dev(0),
          dbdir_inode(0) {}
};

// Session registry - manages persistent storage of session info
class SessionRegistry {
public:
    SessionRegistry();
    ~SessionRegistry();

    // Load all sessions from registry file
    bool load_all(std::vector<SessionInfo>& sessions);

    // Add a new session to registry
    bool add(const SessionInfo& session);

    // Replace or add a session record
    bool upsert(const SessionInfo& session);

    // Update last active timestamp
    bool touch(int session_id, time_t last_active);

    // Remove a session from registry
    bool remove(int session_id);

    // Get session by ID
    bool get(int session_id, SessionInfo& session);

    // Get the latest session (highest ID)
    bool get_latest(SessionInfo& session);

    // Get next available session ID
    int get_next_id();

    // Clean up stale sessions (dead processes)
    bool cleanup_stale();

    // Clear all sessions
    bool clear_all();

private:
    char registry_path_[256];

    // File locking for concurrent access
    bool lock_file(int fd);
    bool unlock_file(int fd);

    // Parse a single line from registry file
    bool parse_line(const char* line, SessionInfo& session);

    // Serialize session to string
    std::string serialize(const SessionInfo& session);
};

} // namespace xtrace
