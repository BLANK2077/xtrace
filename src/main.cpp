#include "commands/cmd_session.h"
#include "commands/cmd_trace.h"
#include "commands/cmd_ai.h"
#include "server/server.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace xtrace;

int main(int argc, char** argv) {
    if (argc < 2) {
        print_help(argv[0]);
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--debug") == 0) {
            setenv("XTRACE_DEBUG", "1", 1);
            break;
        }
    }

    // Check for server mode first
    if (strcmp(argv[1], "--server") == 0) {
        // Strip "--server": pass argv[0], argv[2..] to server_main
        std::vector<char*> srv_argv;
        srv_argv.push_back(argv[0]);
        for (int i = 2; i < argc; i++)
            srv_argv.push_back(argv[i]);
        srv_argv.push_back(nullptr);
        return server_main((int)srv_argv.size() - 1, srv_argv.data());
    }

    const char* cmd = argv[1];

    // Help
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_help(argv[0]);
        return 0;
    }

    // Open - create new session
    if (strcmp(cmd, "open") == 0) {
        return cmd_open(argc, argv);
    }

    // Session commands
    if (strcmp(cmd, "session") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s session <list|ensure|kill|doctor>\n", argv[0]);
            return 1;
        }

        const char* subcmd = argv[2];

        if (strcmp(subcmd, "list") == 0) {
            return cmd_session_list();
        }

        if (strcmp(subcmd, "ensure") == 0) {
            return cmd_session_ensure(argc, argv);
        }

        if (strcmp(subcmd, "kill") == 0) {
            if (argc < 4) {
                fprintf(stderr, "Usage: %s session kill <id|all>\n", argv[0]);
                return 1;
            }
            return cmd_session_kill(argv[3]);
        }

        if (strcmp(subcmd, "doctor") == 0) {
            return cmd_session_doctor(argc, argv);
        }

        fprintf(stderr, "Unknown session subcommand: %s\n", subcmd);
        return 1;
    }

    if (strcmp(cmd, "ai") == 0) {
        return cmd_ai(argc, argv);
    }

    // Close - close latest session
    if (strcmp(cmd, "close") == 0) {
        return cmd_close();
    }

    // Driver / Load tracing
    if (strcmp(cmd, "driver") == 0) {
        return cmd_driver(argc, argv);
    }
    if (strcmp(cmd, "load") == 0) {
        return cmd_load(argc, argv);
    }

    if (strcmp(cmd, "signal") == 0) {
        return cmd_signal(argc, argv);
    }

    if (strcmp(cmd, "query") == 0) {
        return cmd_query(argc, argv);
    }

    // Unknown command
    fprintf(stderr, "Unknown command: %s\n\n", cmd);
    print_help(argv[0]);
    return 1;
}
