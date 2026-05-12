#pragma once

namespace xtrace {

// Command implementations

// xtrace open - create a new session
int cmd_open(int argc, char** argv);

// xtrace session list - list all sessions
int cmd_session_list();

// xtrace session kill <id> - kill a session
int cmd_session_kill(const char* id_str);

// xtrace session doctor -s <sid> [-json] - diagnose a session
int cmd_session_doctor(int argc, char** argv);

// xtrace close - close latest session
int cmd_close();

// Print help message
void print_help(const char* prog);

} // namespace xtrace
