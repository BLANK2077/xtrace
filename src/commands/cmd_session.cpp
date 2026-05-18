#include "cmd_session.h"
#include "../session/session_manager.h"
#include "../client/client.h"
#include "../protocol/protocol.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <ctime>

namespace xtrace {

using json = nlohmann::json;

static std::string format_epoch(time_t t) {
    if (t <= 0) return "-";
    char buf[64];
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static bool has_json_arg(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-json") == 0) {
            return true;
        }
    }
    return false;
}

static bool is_debug_arg(const char* arg) {
    return strcmp(arg, "--debug") == 0;
}

void print_help(const char* prog) {
    printf("XTrace - NPI-based Signal Tracing Tool\n\n");
    printf("Usage:\n");
    printf("  %s open -dbdir <simv.daidir> --name <name> [args...]  Load design and create named session\n", prog);
    printf("  %s open -dbdir <simv.daidir> --name <name> --debug  Print session startup diagnostics\n", prog);
    printf("  %s session list        List all active sessions\n", prog);
    printf("  %s session ensure -dbdir <simv.daidir> --name <name> [-json] [args...]  Create named session\n", prog);
    printf("  %s session kill <name> Kill a specific session\n", prog);
    printf("  %s session kill all    Kill all sessions\n", prog);
    printf("  %s session doctor -s <name> [-json] [--debug]  Diagnose a session\n", prog);
    printf("  %s driver <sig> -s <name> [-json]  Trace signal drivers\n", prog);
    printf("  %s load   <sig> -s <name> [-json]  Trace signal loads\n", prog);
    printf("  %s signal resolve <signal> -s <name> [-json]\n", prog);
    printf("  %s query -dbdir <simv.daidir> --name <name> <--driver|--load> <sig> [-json] [filters]\n", prog);
    printf("  %s ai <query|schema|actions> ...  AI JSON interface\n", prog);
    printf("  %s close               Close the latest-created session\n", prog);
    printf("  %s help                Show this help\n", prog);
    printf("\nExamples:\n");
    printf("  %s open -dbdir simv.daidir --name case_a\n", prog);
    printf("  %s session ensure -dbdir simv.daidir --name case_a -json\n", prog);
    printf("  %s ai query --json '{\"api_version\":\"xtrace.ai.v1\",\"action\":\"trace.driver\",...}'\n", prog);
    printf("  %s session list\n", prog);
    printf("  %s session kill case_a\n", prog);
    printf("\nDebug:\n");
    printf("  Use --debug or XTRACE_DEBUG=1 to print session lifecycle diagnostics to stderr.\n");
    printf("  Server debug logs are written to ~/.xtrace/sessions/<hashed-name>/debug.log.\n");
}

int cmd_open(int argc, char** argv) {
    if (argc < 4 || strcmp(argv[2], "-dbdir") != 0) {
        fprintf(stderr, "Usage: %s open -dbdir <simv.daidir> --name <name> [args...]\n", argv[0]);
        return 1;
    }

    std::string session_name;
    std::vector<std::string> design_args;
    for (int i = 2; i < argc; i++) {
        if (is_debug_arg(argv[i])) continue;
        if ((strcmp(argv[i], "--name") == 0 || strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
            session_name = argv[++i];
            continue;
        }
        design_args.push_back(argv[i]);
    }
    if (session_name.empty()) {
        fprintf(stderr, "Error: session name is required. Use --name <name>.\n");
        return 1;
    }

    SessionManager manager;
    SessionEnsureResult result = manager.create_session(design_args, session_name);

    if (!result.ok) {
        fprintf(stderr, "Error: %s (status=%s)\n",
                result.message.empty() ? "Failed to create session" : result.message.c_str(),
                result.status.empty() ? "error" : result.status.c_str());
        return 1;
    }

    printf("[Session %s] Database loaded: %s\n", result.session_id.c_str(), result.info.dbdir_path.c_str());
    return 0;
}

int cmd_session_ensure(int argc, char** argv) {
    bool json_output = has_json_arg(argc, argv);
    std::vector<std::string> design_args;
    std::string session_name;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-json") == 0) {
            continue;
        } else if (is_debug_arg(argv[i])) {
            continue;
        } else if ((strcmp(argv[i], "--name") == 0 || strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
            session_name = argv[++i];
        } else {
            design_args.push_back(argv[i]);
        }
    }

    SessionManager manager;
    SessionEnsureResult result = manager.ensure_session(design_args, session_name);

    if (json_output) {
        json payload = {
            {"ok", result.ok},
            {"id", result.session_id},
            {"session_id", result.session_id},
            {"status", result.status.empty() ? (result.ok ? "healthy" : "error") : result.status},
            {"reused", result.reused},
            {"dbdir_path", result.info.dbdir_path},
            {"message", result.message}
        };
        printf("%s\n", payload.dump(2).c_str());
    } else if (result.ok) {
        printf("[Session %s] %s: %s\n",
               result.session_id.c_str(),
               "Database loaded",
               result.info.dbdir_path.c_str());
    } else {
        fprintf(stderr, "Error: %s (status=%s)\n",
                result.message.c_str(),
                result.status.empty() ? "error" : result.status.c_str());
    }

    return result.ok ? 0 : 1;
}

int cmd_session_list() {
    SessionManager manager;
    std::vector<SessionInfo> sessions = manager.list_sessions();

    if (sessions.empty()) {
        printf("No active sessions.\n");
        return 0;
    }

    printf("ID                   | PID     | Created             | Last Active         | Daidir                    | Socket Path\n");
    printf("---------------------|---------|---------------------|---------------------|---------------------------|------------------------------\n");

    for (const auto& s : sessions) {
        // Truncate design name if too long
        std::string dbdir = s.dbdir_path.empty() ? s.design_file : s.dbdir_path;
        if (dbdir.length() > 25) {
            dbdir = "..." + dbdir.substr(dbdir.length() - 22);
        }
        printf("%-20s | %-7d | %-19s | %-19s | %-25s | %s\n",
               s.session_id.c_str(),
               s.server_pid,
               format_epoch(s.created_at).c_str(),
               format_epoch(s.last_active).c_str(),
               dbdir.c_str(),
               s.socket_path.c_str());
    }

    printf("\nTotal: %zu session(s)\n", sessions.size());
    return 0;
}

int cmd_session_kill(const char* id_str) {
    if (strcmp(id_str, "all") == 0) {
        SessionManager manager;
        printf("Killing all sessions...\n");
        manager.kill_all_sessions();
        printf("All sessions killed.\n");
        return 0;
    }

    std::string session_id = id_str ? id_str : "";
    SessionManager manager;
    SessionHealth health = manager.diagnose_session(session_id);
    if (health.status == SessionHealthStatus::RegistryMissing) {
        fprintf(stderr, "Error: Session %s is not in registry\n", session_id.c_str());
        return 1;
    }

    if (health.healthy) {
        printf("Killing session %s...\n", session_id.c_str());
    } else {
        printf("Cleaning stale session %s (%s: %s)...\n",
               session_id.c_str(),
               session_health_status_name(health.status),
               health.message.c_str());
    }

    if (manager.kill_session(session_id)) {
        printf("Session %s removed.\n", session_id.c_str());
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to kill session %s\n", session_id.c_str());
        return 1;
    }
}

int cmd_session_doctor(int argc, char** argv) {
    std::string session_id;
    bool json_output = false;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            session_id = argv[++i];
        } else if (strcmp(argv[i], "-json") == 0) {
            json_output = true;
        } else if (is_debug_arg(argv[i])) {
            continue;
        } else {
            fprintf(stderr, "Usage: %s session doctor -s <name> [-json] [--debug]\n", argv[0]);
            return 1;
        }
    }

    if (session_id.empty()) {
        fprintf(stderr, "Usage: %s session doctor -s <name> [-json] [--debug]\n", argv[0]);
        fprintf(stderr, "Error: session doctor requires -s <name>\n");
        return 1;
    }

    SessionManager manager;
    SessionHealth health = manager.diagnose_session(session_id);
    const char* status = session_health_status_name(health.status);

    if (json_output) {
        json payload = {
            {"id", health.session_id},
            {"session_id", health.session_id},
            {"healthy", health.healthy},
            {"status", status},
            {"message", health.message},
            {"pid", health.info.server_pid},
            {"socket_path", health.info.socket_path},
            {"design_file", health.info.design_file},
            {"dbdir_path", health.info.dbdir_path},
            {"dbdir_mtime", health.info.dbdir_mtime},
            {"dbdir_size", health.info.dbdir_size},
            {"dbdir_dev", health.info.dbdir_dev},
            {"dbdir_inode", health.info.dbdir_inode}
        };
        printf("%s\n", payload.dump(2).c_str());
    } else if (health.healthy) {
        printf("Session %s healthy\n", session_id.c_str());
        printf("  status: %s\n", status);
        printf("  pid: %d\n", health.info.server_pid);
        printf("  socket_path: %s\n", health.info.socket_path.c_str());
        printf("  design_file: %s\n", health.info.design_file.c_str());
        printf("  dbdir_path: %s\n", health.info.dbdir_path.c_str());
    } else {
        printf("Session %s unhealthy\n", session_id.c_str());
        printf("  status: %s\n", status);
        printf("  message: %s\n", health.message.c_str());
        if (!health.info.session_id.empty()) {
            printf("  pid: %d\n", health.info.server_pid);
            printf("  socket_path: %s\n", health.info.socket_path.c_str());
            printf("  design_file: %s\n", health.info.design_file.c_str());
            printf("  dbdir_path: %s\n", health.info.dbdir_path.c_str());
        }
    }

    return health.healthy ? 0 : 1;
}

int cmd_close() {
    SessionManager manager;
    SessionInfo session;

    if (!manager.get_latest_session(session)) {
        fprintf(stderr, "Error: No active sessions\n");
        return 1;
    }

    printf("Closing session %s...\n", session.session_id.c_str());
    if (manager.kill_session(session.session_id)) {
        printf("Session %s closed.\n", session.session_id.c_str());
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to close session %s\n", session.session_id.c_str());
        return 1;
    }
}

} // namespace xtrace
