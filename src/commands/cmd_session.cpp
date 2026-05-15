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

void print_help(const char* prog) {
    printf("XTrace - NPI-based Signal Tracing Tool\n\n");
    printf("Usage:\n");
    printf("  %s open -dbdir <simv.daidir> [args...]  Load design and create new session\n", prog);
    printf("  %s session list        List all active sessions\n", prog);
    printf("  %s session ensure -dbdir <simv.daidir> [-json] [args...]  Ensure healthy session\n", prog);
    printf("  %s session kill <id>   Kill a specific session\n", prog);
    printf("  %s session kill all    Kill all sessions\n", prog);
    printf("  %s session doctor -s <sid> [-json]  Diagnose a session\n", prog);
    printf("  %s driver <sig> [-s <sid>] [-json]  Trace signal drivers\n", prog);
    printf("  %s load   <sig> [-s <sid>] [-json]  Trace signal loads\n", prog);
    printf("  %s signal <resolve|search> <pattern> -s <sid> [-json] [--limit N]\n", prog);
    printf("  %s query -dbdir <simv.daidir> <--driver|--load> <sig> [-json] [filters]\n", prog);
    printf("  %s ai <query|schema|actions> ...  AI JSON interface\n", prog);
    printf("  %s close               Close the latest session\n", prog);
    printf("  %s help                Show this help\n", prog);
    printf("\nExamples:\n");
    printf("  %s open -dbdir simv.daidir\n", prog);
    printf("  %s session ensure -dbdir simv.daidir -json\n", prog);
    printf("  %s ai query --json '{\"api_version\":\"xtrace.ai.v1\",\"action\":\"trace.driver\",...}'\n", prog);
    printf("  %s session list\n", prog);
    printf("  %s session kill 1\n", prog);
}

int cmd_open(int argc, char** argv) {
    if (argc < 4 || strcmp(argv[2], "-dbdir") != 0) {
        fprintf(stderr, "Usage: %s open -dbdir <simv.daidir> [args...]\n", argv[0]);
        return 1;
    }

    // Collect design args (skip "open")
    std::vector<std::string> design_args;
    for (int i = 2; i < argc; i++) {
        design_args.push_back(argv[i]);
    }

    SessionManager manager;
    int session_id = manager.create_session(design_args);

    if (session_id <= 0) {
        fprintf(stderr, "Error: Failed to create session\n");
        return 1;
    }

    printf("[Session %d] Database loaded: %s\n", session_id, argv[3]);
    return 0;
}

int cmd_session_ensure(int argc, char** argv) {
    bool json_output = has_json_arg(argc, argv);
    std::vector<std::string> design_args;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-json") == 0) {
            continue;
        } else {
            design_args.push_back(argv[i]);
        }
    }

    SessionManager manager;
    SessionEnsureResult result = manager.ensure_session(design_args);

    if (json_output) {
        json payload = {
            {"ok", result.ok},
            {"session_id", result.session_id},
            {"status", result.status.empty() ? (result.ok ? "healthy" : "error") : result.status},
            {"reused", result.reused},
            {"dbdir_path", result.info.dbdir_path},
            {"message", result.message}
        };
        printf("%s\n", payload.dump(2).c_str());
    } else if (result.ok) {
        printf("[Session %d] %s: %s\n",
               result.session_id,
               result.reused ? "Reused" : "Database loaded",
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

    printf("ID  | PID     | Created             | Last Active         | Daidir                    | Socket Path\n");
    printf("----|---------|---------------------|---------------------|---------------------------|------------------------------\n");

    for (const auto& s : sessions) {
        // Truncate design name if too long
        std::string dbdir = s.dbdir_path.empty() ? s.design_file : s.dbdir_path;
        if (dbdir.length() > 25) {
            dbdir = "..." + dbdir.substr(dbdir.length() - 22);
        }
        printf("%-3d | %-7d | %-19s | %-19s | %-25s | %s\n",
               s.session_id,
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

    int session_id = atoi(id_str);
    if (session_id <= 0) {
        fprintf(stderr, "Error: Invalid session ID: %s\n", id_str);
        return 1;
    }

    SessionManager manager;
    SessionHealth health = manager.diagnose_session(session_id);
    if (health.status == SessionHealthStatus::RegistryMissing) {
        fprintf(stderr, "Error: Session %d is not in registry\n", session_id);
        return 1;
    }

    if (health.healthy) {
        printf("Killing session %d...\n", session_id);
    } else {
        printf("Cleaning stale session %d (%s: %s)...\n",
               session_id,
               session_health_status_name(health.status),
               health.message.c_str());
    }

    if (manager.kill_session(session_id)) {
        printf("Session %d removed.\n", session_id);
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to kill session %d\n", session_id);
        return 1;
    }
}

int cmd_session_doctor(int argc, char** argv) {
    int session_id = -1;
    bool json_output = false;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            session_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-json") == 0) {
            json_output = true;
        } else {
            fprintf(stderr, "Usage: %s session doctor -s <sid> [-json]\n", argv[0]);
            return 1;
        }
    }

    if (session_id <= 0) {
        fprintf(stderr, "Usage: %s session doctor -s <sid> [-json]\n", argv[0]);
        fprintf(stderr, "Error: session doctor requires -s <sid>\n");
        return 1;
    }

    SessionManager manager;
    SessionHealth health = manager.diagnose_session(session_id);
    const char* status = session_health_status_name(health.status);

    if (json_output) {
        json payload = {
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
        printf("Session %d healthy\n", session_id);
        printf("  status: %s\n", status);
        printf("  pid: %d\n", health.info.server_pid);
        printf("  socket_path: %s\n", health.info.socket_path.c_str());
        printf("  design_file: %s\n", health.info.design_file.c_str());
        printf("  dbdir_path: %s\n", health.info.dbdir_path.c_str());
    } else {
        printf("Session %d unhealthy\n", session_id);
        printf("  status: %s\n", status);
        printf("  message: %s\n", health.message.c_str());
        if (health.info.session_id > 0) {
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

    printf("Closing session %d...\n", session.session_id);
    if (manager.kill_session(session.session_id)) {
        printf("Session %d closed.\n", session.session_id);
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to close session %d\n", session.session_id);
        return 1;
    }
}

} // namespace xtrace
