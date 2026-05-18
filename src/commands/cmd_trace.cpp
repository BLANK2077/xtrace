#include "cmd_trace.h"
#include "../session/session_manager.h"
#include "../client/client.h"
#include "../protocol/protocol.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace xtrace {

using json = nlohmann::json;

static void print_command_json_error(const char* command,
                                     const std::string& session_id,
                                     const char* status,
                                     const std::string& message) {
    json payload = {
        {"ok", false},
        {"command", command ? command : ""},
        {"session_id", session_id}, {"id", session_id},
        {"status", status ? status : "error"},
        {"message", message}
    };
    fprintf(stderr, "%s\n", payload.dump(2).c_str());
}

static bool json_payload_ok(const std::string& payload) {
    try {
        json parsed = json::parse(payload);
        if (parsed.contains("ok")) {
            return parsed["ok"].get<bool>();
        }
    } catch (...) {
    }
    return true;
}

static bool has_json_arg(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-json") == 0) {
            return true;
        }
    }
    return false;
}

static std::string build_trace_command(bool is_driver,
                                       bool json_output,
                                       const std::string& signal,
                                       int limit,
                                       const std::string& role,
                                       bool no_statement_only) {
    std::string cmd;
    if (is_driver) {
        cmd = json_output ? CMD_DRIVER_JSON : CMD_DRIVER;
    } else {
        cmd = json_output ? CMD_LOAD_JSON : CMD_LOAD;
    }
    cmd += " " + signal;
    if (limit > 0) {
        cmd += " --limit " + std::to_string(limit);
    }
    if (!role.empty()) {
        cmd += " --role " + role;
    }
    if (no_statement_only) {
        cmd += " --no-statement-only";
    }
    return cmd;
}

static int cmd_trace(int argc, char** argv, bool is_driver) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s %s <signal> -s <name> [-json]\n",
                argv[0], is_driver ? "driver" : "load");
        return 1;
    }

    std::string signal = argv[2];
    std::string session_id;
    bool json_output = has_json_arg(argc, argv);
    int limit = 0;
    std::string role;
    bool no_statement_only = false;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            session_id = argv[++i];
        } else if (strcmp(argv[i], "-json") == 0) {
            continue;
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            role = argv[++i];
        } else if (strcmp(argv[i], "--no-statement-only") == 0) {
            no_statement_only = true;
        } else {
            if (json_output) {
                print_command_json_error(is_driver ? "driver" : "load", session_id, "invalid_args",
                                         std::string("Unknown option: ") + argv[i]);
            } else {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
            }
            return 1;
        }
    }

    SessionManager manager;
    SessionInfo session;
    if (!session_id.empty()) {
        if (!manager.get_session(session_id, session)) {
            SessionHealth health = manager.diagnose_session(session_id);
            if (json_output) {
                print_command_json_error(is_driver ? "driver" : "load",
                                         session_id,
                                         session_health_status_name(health.status),
                                         health.message);
            } else {
                fprintf(stderr, "Error: Session %s unavailable: %s (status=%s)\n",
                        session_id.c_str(),
                        health.message.c_str(),
                        session_health_status_name(health.status));
            }
            return 1;
        }
    } else {
        if (json_output) {
            print_command_json_error(is_driver ? "driver" : "load", session_id, "missing_session", "-s <name> is required");
        } else {
            fprintf(stderr, "Error: -s <name> is required\n");
        }
        return 1;
    }

    std::string cmd = build_trace_command(is_driver, json_output, signal, limit, role, no_statement_only);

    if (json_output) {
        std::string payload;
        std::string status;
        std::string message;
        if (!send_command_capture(session_id, cmd.c_str(), payload, status, message)) {
            print_command_json_error(is_driver ? "driver" : "load", session_id, status.c_str(), message);
            return 1;
        }
        fwrite(payload.c_str(), 1, payload.size(), stdout);
        return json_payload_ok(payload) ? 0 : 1;
    }

    if (!send_command_and_print_ex(session_id, cmd.c_str(), false, is_driver ? "driver" : "load")) {
        fprintf(stderr, "Error: Failed to send command to session %s\n", session_id.c_str());
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

int cmd_signal(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s signal resolve <signal> -s <name> [-json]\n", argv[0]);
        return 1;
    }

    bool resolve = strcmp(argv[2], "resolve") == 0;
    if (!resolve) {
        fprintf(stderr, "Usage: %s signal resolve <signal> -s <name> [-json]\n", argv[0]);
        return 1;
    }

    std::string pattern = argv[3];
    std::string session_id;
    bool json_output = has_json_arg(argc, argv);

    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            session_id = argv[++i];
        } else if (strcmp(argv[i], "-json") == 0) {
            continue;
        } else {
            if (json_output) {
                print_command_json_error("signal", session_id, "invalid_args", std::string("Unknown option: ") + argv[i]);
            } else {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
            }
            return 1;
        }
    }

    if (session_id.empty()) {
        if (json_output) {
            print_command_json_error("signal", session_id, "invalid_args", "signal requires -s <name>");
        } else {
            fprintf(stderr, "Usage: %s signal resolve <signal> -s <name> [-json]\n", argv[0]);
        }
        return 1;
    }

    std::string cmd = json_output ? CMD_SIGNAL_RESOLVE : CMD_SIGNAL_RESOLVE_TEXT;
    cmd += " " + pattern;

    std::string payload;
    std::string status;
    std::string message;
    if (!send_command_capture(session_id, cmd.c_str(), payload, status, message)) {
        if (json_output) {
            print_command_json_error("signal", session_id, status.c_str(), message);
        } else {
            fprintf(stderr, "Error: %s (status=%s)\n", message.c_str(), status.c_str());
        }
        return 1;
    }
    fwrite(payload.c_str(), 1, payload.size(), stdout);
    if (json_output) {
        return json_payload_ok(payload) ? 0 : 1;
    }
    return payload.find("Error: ") == 0 ? 1 : 0;
}

int cmd_query(int argc, char** argv) {
    std::vector<std::string> design_args;
    std::string signal;
    bool is_driver = true;
    bool have_mode = false;
    bool json_output = has_json_arg(argc, argv);
    int limit = 0;
    std::string role;
    bool no_statement_only = false;
    std::string session_name;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-dbdir") == 0 && i + 1 < argc) {
            design_args.push_back(argv[i]);
            design_args.push_back(argv[++i]);
        } else if (strcmp(argv[i], "--driver") == 0 && i + 1 < argc) {
            is_driver = true;
            have_mode = true;
            signal = argv[++i];
        } else if (strcmp(argv[i], "--load") == 0 && i + 1 < argc) {
            is_driver = false;
            have_mode = true;
            signal = argv[++i];
        } else if ((strcmp(argv[i], "--name") == 0 || strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
            session_name = argv[++i];
        } else if (strcmp(argv[i], "-json") == 0) {
            continue;
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            role = argv[++i];
        } else if (strcmp(argv[i], "--no-statement-only") == 0) {
            no_statement_only = true;
        } else {
            if (json_output) {
                print_command_json_error("query", "", "invalid_args", std::string("Unknown option: ") + argv[i]);
            } else {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
            }
            return 1;
        }
    }

    if (design_args.empty() || session_name.empty() || !have_mode || signal.empty()) {
        const char* usage = "Usage: query -dbdir <simv.daidir> --name <name> <--driver|--load> <signal> [-json] [filters]";
        if (json_output) {
            print_command_json_error("query", "", "invalid_args", usage);
        } else {
            fprintf(stderr, "%s\n", usage);
        }
        return 1;
    }

    SessionManager manager;
    SessionEnsureResult ensure = manager.ensure_session(design_args, session_name);
    if (!ensure.ok) {
        if (json_output) {
            json payload = {
                {"ok", false},
                {"command", "query"},
                {"status", ensure.status},
                {"message", ensure.message},
                {"session", {
                    {"ok", false},
                    {"session_id", ensure.session_id},
                    {"reused", ensure.reused},
                    {"dbdir_path", ensure.info.dbdir_path}
                }}
            };
            fprintf(stderr, "%s\n", payload.dump(2).c_str());
        } else {
            fprintf(stderr, "Error: %s (status=%s)\n", ensure.message.c_str(), ensure.status.c_str());
        }
        return 1;
    }

    std::string trace_cmd = build_trace_command(is_driver, json_output, signal, limit, role, no_statement_only);
    std::string payload;
    std::string status;
    std::string message;
    if (!send_command_capture(ensure.session_id, trace_cmd.c_str(), payload, status, message)) {
        if (json_output) {
            print_command_json_error("query", ensure.session_id, status.c_str(), message);
        } else {
            fprintf(stderr, "Error: %s (status=%s)\n", message.c_str(), status.c_str());
        }
        return 1;
    }

    if (json_output) {
        json trace = json::parse(payload);
        json combined = {
            {"ok", trace.value("ok", true)},
            {"session", {
                {"ok", true},
                {"session_id", ensure.session_id},
                {"status", ensure.status},
                {"reused", ensure.reused},
                {"dbdir_path", ensure.info.dbdir_path},
                {"message", ensure.message}
            }},
            {"trace", trace}
        };
        printf("%s\n", combined.dump(2).c_str());
        return combined["ok"].get<bool>() ? 0 : 1;
    }

    printf("[Session %s] %s: %s\n",
           ensure.session_id.c_str(),
           "Database loaded",
           ensure.info.dbdir_path.c_str());
    fwrite(payload.c_str(), 1, payload.size(), stdout);
    return 0;
}

} // namespace xtrace
