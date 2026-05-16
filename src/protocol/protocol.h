#pragma once

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/xtrace_paths.h"

#define PROTOCOL_VERSION    "1.2"

// Socket path configuration
#define SOCK_PATH_PREFIX    ".xtrace"
#define SOCK_PATH_LEN       256
#define REGISTRY_FILE       "registry.json"

// Protocol commands (client -> server)
#define CMD_PING            "PING"
#define CMD_VERSION         "VERSION"
#define CMD_QUIT            "QUIT"
#define CMD_DRIVER          "DRIVER"
#define CMD_LOAD            "LOAD"
#define CMD_DRIVER_JSON     "DRIVER_JSON"
#define CMD_LOAD_JSON       "LOAD_JSON"
#define CMD_DRIVER_AI       "DRIVER_AI"
#define CMD_LOAD_AI         "LOAD_AI"
#define CMD_SIGNAL_RESOLVE  "SIGNAL_RESOLVE"
#define CMD_SIGNAL_RESOLVE_TEXT "SIGNAL_RESOLVE_TEXT"
#define CMD_PORT_TRACE_AI       "PORT_TRACE_AI"
#define CMD_INSTANCE_MAP_AI     "INSTANCE_MAP_AI"
#define CMD_INTERFACE_RESOLVE_AI "INTERFACE_RESOLVE_AI"

// End-of-response marker (server -> client)
#define END_MARKER          "##END##\n"
#define ERROR_PREFIX        "ERROR: "

// Get socket path for a given session ID
inline void get_sock_path(char* buf, int session_id) {
    snprintf(buf, SOCK_PATH_LEN, "%s", xtrace::xtrace_socket_path(session_id).c_str());
}

// Get registry file path
inline void get_registry_path(char* buf) {
    snprintf(buf, SOCK_PATH_LEN, "%s", xtrace::xtrace_registry_path().c_str());
}

inline void get_registry_lock_path(char* buf) {
    snprintf(buf, SOCK_PATH_LEN, "%s", xtrace::xtrace_registry_lock_path().c_str());
}

inline void get_debug_log_path(char* buf, int session_id) {
    snprintf(buf, SOCK_PATH_LEN, "%s", xtrace::xtrace_debug_log_path(session_id).c_str());
}
