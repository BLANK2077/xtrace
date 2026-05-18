#pragma once

#include <string>

namespace xtrace {

// Client communication functions

// Connect to a session's server, returns fd or -1 on error
int session_connect(const std::string& session_id);

// Send command and print response
bool send_command_and_print(const std::string& session_id, const char* cmd);

// Send command and print response, optionally formatting transport/server errors as JSON
bool send_command_and_print_ex(const std::string& session_id, const char* cmd, bool json_errors, const char* command_name);

// Send command and capture server payload without printing.
bool send_command_capture(const std::string& session_id,
                          const char* cmd,
                          std::string& payload,
                          std::string& status,
                          std::string& message);

// Check if session is responsive (sends PING)
bool session_ping(const std::string& session_id);

} // namespace xtrace
