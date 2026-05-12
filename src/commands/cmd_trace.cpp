#include "cmd_trace.h"
#include "../session/session_manager.h"
#include "../client/client.h"
#include "../protocol/protocol.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace xtrace {

static int cmd_trace(int argc, char** argv, bool is_driver) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s %s <signal> [-s <session_id>]\n",
                argv[0], is_driver ? "driver" : "load");
        return 1;
    }

    std::string signal = argv[2];
    int session_id = -1;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            session_id = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    SessionManager manager;
    SessionInfo session;
    if (session_id > 0) {
        if (!manager.get_session(session_id, session)) {
            SessionHealth health = manager.diagnose_session(session_id);
            fprintf(stderr, "Error: Session %d unavailable: %s (status=%s)\n",
                    session_id,
                    health.message.c_str(),
                    session_health_status_name(health.status));
            return 1;
        }
    } else {
        if (!manager.get_latest_session(session)) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        session_id = session.session_id;
    }

    std::string cmd = is_driver ? CMD_DRIVER : CMD_LOAD;
    cmd += " " + signal;

    if (!send_command_and_print(session_id, cmd.c_str())) {
        fprintf(stderr, "Error: Failed to send command to session %d\n", session_id);
        return 1;
    }
    return 0;
}

int cmd_driver(int argc, char** argv) {
    return cmd_trace(argc, argv, true);
}

int cmd_load(int argc, char** argv) {
    return cmd_trace(argc, argv, false);
}

} // namespace xtrace
