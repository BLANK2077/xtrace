#pragma once

#include <string>

namespace xtrace {

std::string xtrace_home_dir();
std::string xtrace_sessions_dir();
std::string xtrace_session_dir(int session_id);
std::string xtrace_registry_path();
std::string xtrace_registry_lock_path();
std::string xtrace_session_json_path(int session_id);
std::string xtrace_socket_path(int session_id);
std::string xtrace_debug_log_path(int session_id);
std::string xtrace_legacy_registry_path();

bool xtrace_ensure_home();
bool xtrace_ensure_session_dir(int session_id);
bool xtrace_remove_session_dir(int session_id);

} // namespace xtrace
