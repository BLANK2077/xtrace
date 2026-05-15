#include "server.h"
#include "../protocol/protocol.h"
#include "../trace/trace_engine.h"
#include "../signal/signal_finder.h"
#include "../port/port_analyzer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
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

static std::vector<std::string> split_tokens(const char* text) {
    std::vector<std::string> tokens;
    std::istringstream in(text ? text : "");
    std::string token;
    while (in >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

static TraceOptions parse_trace_options(const std::vector<std::string>& tokens,
                                        size_t start_index) {
    TraceOptions options;
    for (size_t i = start_index; i < tokens.size(); ++i) {
        if (tokens[i] == "--limit" && i + 1 < tokens.size()) {
            options.limit = atoi(tokens[++i].c_str());
        } else if (tokens[i] == "--role" && i + 1 < tokens.size()) {
            options.role = tokens[++i];
        } else if (tokens[i] == "--no-statement-only") {
            options.no_statement_only = true;
        }
    }
    return options;
}

static int parse_limit_option(const std::vector<std::string>& tokens, size_t start_index, int default_limit) {
    int limit = default_limit;
    for (size_t i = start_index; i < tokens.size(); ++i) {
        if (tokens[i] == "--limit" && i + 1 < tokens.size()) {
            limit = atoi(tokens[++i].c_str());
        }
    }
    return limit;
}

static void handle_trace(int client_fd, const char* request, TraceMode mode, bool json_output, bool ai_output = false) {
    std::vector<std::string> tokens = split_tokens(request);
    if (tokens.empty()) {
        const char* err = ERROR_PREFIX "Missing signal\n";
        send_all(client_fd, err, strlen(err));
        send_all(client_fd, END_MARKER, strlen(END_MARKER));
        return;
    }

    std::string signal = tokens[0];
    TraceOptions options = parse_trace_options(tokens, 1);
    TraceEngine engine;
    TraceResult result = engine.trace(signal, mode, options);
    std::string payload = ai_output ? engine.render_ai_json(result) :
                          json_output ? engine.render_json(result) : engine.render_text(result);
    send_all(client_fd, payload.c_str(), payload.size());
    send_all(client_fd, END_MARKER, strlen(END_MARKER));
}

static void handle_signal(int client_fd, const char* request, bool resolve, bool json_output) {
    std::vector<std::string> tokens = split_tokens(request);
    if (tokens.empty()) {
        const char* err = ERROR_PREFIX "Missing signal pattern\n";
        send_all(client_fd, err, strlen(err));
        send_all(client_fd, END_MARKER, strlen(END_MARKER));
        return;
    }

    int limit = parse_limit_option(tokens, 1, 20);
    SignalFinder finder;
    SignalSearchResult result = resolve ? finder.resolve(tokens[0], limit) : finder.search(tokens[0], limit);
    std::string payload = json_output ? finder.render_json(result) : finder.render_text(result);
    send_all(client_fd, payload.c_str(), payload.size());
    send_all(client_fd, END_MARKER, strlen(END_MARKER));
}

static void handle_port_command(int client_fd, const char* request, const std::string& action) {
    std::vector<std::string> tokens = split_tokens(request);
    if (tokens.empty()) {
        const char* err = ERROR_PREFIX "Missing path\n";
        send_all(client_fd, err, strlen(err));
        send_all(client_fd, END_MARKER, strlen(END_MARKER));
        return;
    }
    int limit = parse_limit_option(tokens, 1, 0);
    PortAnalyzer analyzer;
    std::string payload;
    if (action == "port.trace") {
        payload = analyzer.render_port_trace(tokens[0], limit);
    } else if (action == "instance.map") {
        payload = analyzer.render_instance_map(tokens[0]);
    } else {
        payload = analyzer.render_interface_resolve(tokens[0]);
    }
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

    if (strcmp(cmd, CMD_VERSION) == 0) {
        const char* version = PROTOCOL_VERSION "\n" END_MARKER;
        send_all(client_fd, version, strlen(version));
        return true;
    }

    if (strncmp(cmd, CMD_DRIVER_AI, strlen(CMD_DRIVER_AI)) == 0) {
        const char* rest = cmd + strlen(CMD_DRIVER_AI);
        while (*rest == ' ') rest++;

        handle_trace(client_fd, rest, TraceMode::Driver, true, true);
        return true;
    }

    if (strncmp(cmd, CMD_LOAD_AI, strlen(CMD_LOAD_AI)) == 0) {
        const char* rest = cmd + strlen(CMD_LOAD_AI);
        while (*rest == ' ') rest++;

        handle_trace(client_fd, rest, TraceMode::Load, true, true);
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

    if (strncmp(cmd, CMD_SIGNAL_RESOLVE_TEXT, strlen(CMD_SIGNAL_RESOLVE_TEXT)) == 0) {
        const char* rest = cmd + strlen(CMD_SIGNAL_RESOLVE_TEXT);
        while (*rest == ' ') rest++;

        handle_signal(client_fd, rest, true, false);
        return true;
    }

    if (strncmp(cmd, CMD_SIGNAL_SEARCH_TEXT, strlen(CMD_SIGNAL_SEARCH_TEXT)) == 0) {
        const char* rest = cmd + strlen(CMD_SIGNAL_SEARCH_TEXT);
        while (*rest == ' ') rest++;

        handle_signal(client_fd, rest, false, false);
        return true;
    }

    if (strncmp(cmd, CMD_SIGNAL_RESOLVE, strlen(CMD_SIGNAL_RESOLVE)) == 0) {
        const char* rest = cmd + strlen(CMD_SIGNAL_RESOLVE);
        while (*rest == ' ') rest++;

        handle_signal(client_fd, rest, true, true);
        return true;
    }

    if (strncmp(cmd, CMD_SIGNAL_SEARCH, strlen(CMD_SIGNAL_SEARCH)) == 0) {
        const char* rest = cmd + strlen(CMD_SIGNAL_SEARCH);
        while (*rest == ' ') rest++;

        handle_signal(client_fd, rest, false, true);
        return true;
    }

    if (strncmp(cmd, CMD_PORT_TRACE_AI, strlen(CMD_PORT_TRACE_AI)) == 0) {
        const char* rest = cmd + strlen(CMD_PORT_TRACE_AI);
        while (*rest == ' ') rest++;
        handle_port_command(client_fd, rest, "port.trace");
        return true;
    }

    if (strncmp(cmd, CMD_INSTANCE_MAP_AI, strlen(CMD_INSTANCE_MAP_AI)) == 0) {
        const char* rest = cmd + strlen(CMD_INSTANCE_MAP_AI);
        while (*rest == ' ') rest++;
        handle_port_command(client_fd, rest, "instance.map");
        return true;
    }

    if (strncmp(cmd, CMD_INTERFACE_RESOLVE_AI, strlen(CMD_INTERFACE_RESOLVE_AI)) == 0) {
        const char* rest = cmd + strlen(CMD_INTERFACE_RESOLVE_AI);
        while (*rest == ' ') rest++;
        handle_port_command(client_fd, rest, "interface.resolve");
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

    // Keep session startup quiet so CLI JSON output remains machine-parseable.
    daemonize_io();

    // Initialize NPI
    int result = npi_init(npi_argc, npi_argv);
    if (result == 0) {
        delete[] npi_argv;
        return 1;
    }

    result = npi_load_design(npi_argc, npi_argv);
    if (result == 0) {
        npi_end();
        delete[] npi_argv;
        return 1;
    }

    delete[] npi_argv;

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
