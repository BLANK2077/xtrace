#pragma once

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define PROTOCOL_VERSION    "1.0"

// Socket path configuration
#define SOCK_PATH_PREFIX    ".xtrace"
#define SOCK_PATH_LEN       256
#define REGISTRY_FILE       ".xtrace.registry"

// Protocol commands (client -> server)
#define CMD_PING            "PING"
#define CMD_QUIT            "QUIT"
#define CMD_DRIVER          "DRIVER"
#define CMD_LOAD            "LOAD"

// End-of-response marker (server -> client)
#define END_MARKER          "##END##\n"
#define ERROR_PREFIX        "ERROR: "

// Get socket path for a given session ID
inline void get_sock_path(char* buf, int session_id) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, SOCK_PATH_LEN, "%s/%s.%d.sock", home, SOCK_PATH_PREFIX, session_id);
}

// Get registry file path
inline void get_registry_path(char* buf) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, SOCK_PATH_LEN, "%s/%s", home, REGISTRY_FILE);
}
