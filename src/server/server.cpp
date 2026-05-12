#include "server.h"
#include "../protocol/protocol.h"
#include "../trace/trace_engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>

#include "npi.h"

namespace xtrace {

// Global for cleanup
static int g_session_id = 0;
static int g_srv_fd = -1;
static char g_sock_path[SOCK_PATH_LEN];

static void cleanup_and_exit(int sig) {
    if (g_srv_fd >= 0) {
        close(g_srv_fd);
    }
    if (strlen(g_sock_path) > 0) {
        unlink(g_sock_path);
    }
    // Note: npi_end not called here because signal handler context
    exit(0);
}

static void daemonize_io() {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
}

static bool send_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static void handle_trace(int client_fd, const char* signal, TraceMode mode, bool json_output) {
    TraceEngine engine;
    TraceResult result = engine.trace(signal, mode);
    std::string payload = json_output ? engine.render_json(result) : engine.render_text(result);
    send_all(client_fd, payload.c_str(), payload.size());
    send_all(client_fd, END_MARKER, strlen(END_MARKER));
}

static bool handle_client(int client_fd, bool& should_quit) {
    should_quit = false;

    // Read command line
    char line[1024] = {};
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(line) - 1) {
        ssize_t n = read(client_fd, line + total, 1);
        if (n <= 0) return false;
        if (line[total] == '\n') break;
        total++;
    }
    line[total] = '\0';

    // Trim whitespace
    char* cmd = line;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r' || cmd[len-1] == ' ')) {
        cmd[len-1] = '\0';
        len--;
    }

    // Handle QUIT
    if (strcmp(cmd, CMD_QUIT) == 0) {
        send_all(client_fd, END_MARKER, strlen(END_MARKER));
        should_quit = true;
        return true;
    }

    // Handle PING
    if (strcmp(cmd, CMD_PING) == 0) {
        const char* pong = "PONG\n" END_MARKER;
        send_all(client_fd, pong, strlen(pong));
        return true;
    }

    if (strncmp(cmd, CMD_DRIVER_JSON, strlen(CMD_DRIVER_JSON)) == 0) {
        const char* rest = cmd + strlen(CMD_DRIVER_JSON);
        while (*rest == ' ') rest++;

        handle_trace(client_fd, rest, TraceMode::Driver, true);
        return true;
    }

    if (strncmp(cmd, CMD_LOAD_JSON, strlen(CMD_LOAD_JSON)) == 0) {
        const char* rest = cmd + strlen(CMD_LOAD_JSON);
        while (*rest == ' ') rest++;

        handle_trace(client_fd, rest, TraceMode::Load, true);
        return true;
    }

    // Handle DRIVER
    if (strncmp(cmd, CMD_DRIVER, strlen(CMD_DRIVER)) == 0) {
        const char* rest = cmd + strlen(CMD_DRIVER);
        while (*rest == ' ') rest++;

        handle_trace(client_fd, rest, TraceMode::Driver, false);
        return true;
    }

    // Handle LOAD
    if (strncmp(cmd, CMD_LOAD, strlen(CMD_LOAD)) == 0) {
        const char* rest = cmd + strlen(CMD_LOAD);
        while (*rest == ' ') rest++;

        handle_trace(client_fd, rest, TraceMode::Load, false);
        return true;
    }

    // Unknown command
    const char* err = ERROR_PREFIX "Unknown command\n" END_MARKER;
    send_all(client_fd, err, strlen(err));
    return true;
}

int server_main(int argc, char** argv) {
    // argv: [exe, session_id, ...design_args...]
    if (argc < 2) {
        fprintf(stderr, "Server mode requires session_id argument\n");
        return 1;
    }

    int arg_idx = 1;

    // Parse session ID
    g_session_id = atoi(argv[arg_idx]);
    if (g_session_id <= 0) {
        fprintf(stderr, "Invalid session ID: %s\n", argv[arg_idx]);
        return 1;
    }
    arg_idx++;

    // Build design args for NPI: [exe, ...design_args from arg_idx...]
    int npi_argc = argc - arg_idx + 1;
    char** npi_argv = new char*[npi_argc];
    npi_argv[0] = argv[0];  // exe name
    for (int i = 1; i < npi_argc; i++) {
        npi_argv[i] = argv[arg_idx + i - 1];
    }

    // Redirect stdout to capture NPI init messages, but keep a copy
    int stdout_copy = dup(STDOUT_FILENO);

    // Initialize NPI
    int result = npi_init(npi_argc, npi_argv);
    if (result == 0) {
        dprintf(stdout_copy, "[Session %d] ERROR: npi_init failed\n", g_session_id);
        close(stdout_copy);
        delete[] npi_argv;
        return 1;
    }

    result = npi_load_design(npi_argc, npi_argv);
    if (result == 0) {
        dprintf(stdout_copy, "[Session %d] ERROR: npi_load_design failed\n", g_session_id);
        npi_end();
        close(stdout_copy);
        delete[] npi_argv;
        return 1;
    }

    delete[] npi_argv;

    // Print session ID to indicate successful initialization
    dprintf(stdout_copy, "[Session %d] Ready\n", g_session_id);
    fflush(stdout);
    close(stdout_copy);

    // Now daemonize I/O
    daemonize_io();

    // Set up signal handlers
    signal(SIGTERM, cleanup_and_exit);
    signal(SIGINT, cleanup_and_exit);

    // Create socket
    g_srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_srv_fd < 0) {
        npi_end();
        return 1;
    }

    get_sock_path(g_sock_path, g_session_id);
    unlink(g_sock_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);

    if (bind(g_srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(g_srv_fd);
        npi_end();
        return 1;
    }
    chmod(g_sock_path, 0600);

    if (listen(g_srv_fd, 8) < 0) {
        close(g_srv_fd);
        unlink(g_sock_path);
        npi_end();
        return 1;
    }

    // Accept loop
    while (true) {
        int client_fd = accept(g_srv_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        bool quit = false;
        handle_client(client_fd, quit);
        close(client_fd);

        if (quit) break;
    }

    // Cleanup
    close(g_srv_fd);
    unlink(g_sock_path);
    npi_end();

    return 0;
}

} // namespace xtrace
