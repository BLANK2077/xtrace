#include "cmd_ai.h"

#include "../client/client.h"
#include "../protocol/protocol.h"
#include "../session/session_manager.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace xtrace {

using json = nlohmann::json;

namespace {

const char* API_VERSION = "xtrace.ai.v1";
const char* TOOL_VERSION = "0.1.0";

std::string read_stream(std::istream& in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string read_file(const std::string& path) {
    std::ifstream in(path.c_str());
    if (!in) return "";
    return read_stream(in);
}

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

std::string leaf_name(const std::string& signal) {
    size_t dot = signal.rfind('.');
    std::string leaf = dot == std::string::npos ? signal : signal.substr(dot + 1);
    size_t bracket = leaf.find('[');
    if (bracket != std::string::npos) leaf = leaf.substr(0, bracket);
    return leaf;
}

std::string lower_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back((char)std::tolower((unsigned char)c));
    return out;
}

bool contains_word_like(const std::string& text, const std::string& token) {
    return lower_copy(text).find(lower_copy(token)) != std::string::npos;
}

std::string next_token_after(const std::string& text, const std::string& key) {
    size_t pos = lower_copy(text).find(lower_copy(key));
    if (pos == std::string::npos) return "";
    pos += key.size();
    while (pos < text.size() && std::isspace((unsigned char)text[pos])) pos++;
    size_t begin = pos;
    while (pos < text.size()) {
        char c = text[pos];
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '[' || c == ']')) break;
        pos++;
    }
    return begin < pos ? text.substr(begin, pos - begin) : "";
}

json session_to_json(const SessionInfo& s) {
    return {
        {"id", s.session_id},
        {"session_id", s.session_id},
        {"dbdir", s.dbdir_path},
        {"dbdir_path", s.dbdir_path},
        {"design_file", s.design_file},
        {"pid", s.server_pid},
        {"socket_path", s.socket_path},
        {"created_at", static_cast<long long>(s.created_at)},
        {"last_active", static_cast<long long>(s.last_active)},
        {"dbdir_mtime", s.dbdir_mtime},
        {"dbdir_size", s.dbdir_size},
        {"dbdir_dev", s.dbdir_dev},
        {"dbdir_inode", s.dbdir_inode}
    };
}

json base_response(const json& request, const std::string& action) {
    json response;
    response["api_version"] = API_VERSION;
    response["request_id"] = request.value("request_id", "");
    response["ok"] = true;
    response["action"] = action;
    response["tool"] = {{"name", "xtrace"}, {"version", TOOL_VERSION}};
    response["session"] = nullptr;
    response["summary"] = json::object();
    response["data"] = json::object();
    response["findings"] = json::array();
    response["suggested_next_actions"] = json::array();
    response["warnings"] = json::array();
    response["error"] = nullptr;
    response["meta"] = {{"elapsed_ms", nullptr}, {"truncated", false}};
    return response;
}

json error_response(const json& request,
                    const std::string& action,
                    const std::string& code,
                    const std::string& message,
                    bool recoverable = true,
                    const json& candidates = json::array(),
                    const json& suggested_actions = json::array()) {
    json response = base_response(request, action);
    response["ok"] = false;
    response["data"] = nullptr;
    response["error"] = {
        {"code", code},
        {"message", message},
        {"recoverable", recoverable},
        {"candidates", candidates},
        {"suggested_actions", suggested_actions}
    };
    response["suggested_next_actions"] = suggested_actions;
    return response;
}

std::vector<std::string> target_dbdir_args(const json& request) {
    std::vector<std::string> args;
    if (!request.contains("target") || !request["target"].is_object()) return args;
    const json& target = request["target"];
    std::string dbdir = target.value("dbdir", "");
    if (!dbdir.empty()) {
        args.push_back("-dbdir");
        args.push_back(dbdir);
    }
    return args;
}

bool ensure_target_session(const json& request,
                           json& response,
                           int& session_id,
                           SessionInfo& session,
                           bool allow_latest = false) {
    SessionManager manager;
    const json target = request.value("target", json::object());

    if (target.contains("session_id") && target["session_id"].is_number_integer()) {
        session_id = target["session_id"].get<int>();
        if (!manager.get_session(session_id, session)) {
            SessionHealth health = manager.diagnose_session(session_id);
            response = error_response(request, request.value("action", ""),
                                      "SESSION_NOT_FOUND",
                                      health.message.empty() ? "session not found" : health.message);
            return false;
        }
        response["session"] = session_to_json(session);
        return true;
    }

    std::vector<std::string> design_args = target_dbdir_args(request);
    if (!design_args.empty()) {
        bool auto_ensure = target.value("auto_ensure", true);
        if (!auto_ensure) {
            response = error_response(request, request.value("action", ""),
                                      "INVALID_TARGET",
                                      "target.dbdir requires auto_ensure=true or an existing session_id");
            return false;
        }
        SessionEnsureResult ensured = manager.ensure_session(design_args);
        if (!ensured.ok) {
            response = error_response(request, request.value("action", ""),
                                      "SESSION_UNHEALTHY",
                                      ensured.message.empty() ? "failed to ensure session" : ensured.message);
            return false;
        }
        session_id = ensured.session_id;
        session = ensured.info;
        response["session"] = session_to_json(session);
        response["session"]["reused"] = ensured.reused;
        response["session"]["healthy"] = true;
        return true;
    }

    if (allow_latest && manager.get_latest_session(session)) {
        session_id = session.session_id;
        response["session"] = session_to_json(session);
        return true;
    }

    response = error_response(request, request.value("action", ""),
                              "INVALID_TARGET",
                              "target must contain session_id or dbdir");
    return false;
}

std::string option_string_from_limits_args(const json& request) {
    std::string options;
    const json args = request.value("args", json::object());
    const json limits = request.value("limits", json::object());

    int limit = 0;
    if (limits.contains("max_results")) limit = limits.value("max_results", 0);
    if (limits.contains("max_rows") && limit <= 0) limit = limits.value("max_rows", 0);
    if (args.contains("limit") && args["limit"].is_number_integer()) limit = args["limit"].get<int>();
    if (limit > 0) options += " --limit " + std::to_string(limit);

    std::string role = args.value("role", "");
    if (!role.empty()) options += " --role " + role;
    if (args.value("no_statement_only", false)) options += " --no-statement-only";
    return options;
}

bool send_json_command(int session_id,
                       const std::string& cmd,
                       json& parsed,
                       std::string& error_status,
                       std::string& error_message) {
    std::string payload;
    if (!send_command_capture(session_id, cmd.c_str(), payload, error_status, error_message)) {
        return false;
    }
    try {
        parsed = json::parse(payload);
    } catch (const std::exception& e) {
        error_status = "invalid_json";
        error_message = e.what();
        return false;
    }
    return true;
}

int find_top_level_op(const std::string& s, const std::string& op) {
    int depth = 0;
    for (int i = static_cast<int>(s.size()) - static_cast<int>(op.size()); i >= 0; --i) {
        char c = s[i];
        if (c == ')') depth++;
        else if (c == '(') depth--;
        if (depth == 0 && s.compare(i, op.size(), op) == 0) return i;
    }
    return -1;
}

json parse_expr_ast(const std::string& expr);

json parse_atom(const std::string& text) {
    std::string s = trim(text);
    if (s.empty()) return {{"type", "unknown"}, {"text", ""}};
    if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
        return parse_expr_ast(s.substr(1, s.size() - 2));
    }
    if (s[0] == '!') {
        return {{"op", "not"}, {"args", json::array({parse_expr_ast(s.substr(1))})}};
    }
    bool numeric_or_const = false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '\'' || c == 'h' || c == 'b' || c == 'd' ||
            c == 'x' || c == 'z' || c == '_' || c == '-') {
            numeric_or_const = true;
        } else {
            numeric_or_const = false;
            break;
        }
    }
    if (numeric_or_const || s == "RUN" || s == "IDLE" || s == "BUSY") {
        return {{"type", "const"}, {"value", s}};
    }
    return {{"type", "signal"}, {"name", s}};
}

json parse_expr_ast(const std::string& expr) {
    std::string s = trim(expr);
    if (s.empty()) return {{"type", "unknown"}, {"text", ""}};

    int qpos = find_top_level_op(s, "?");
    if (qpos >= 0) {
        int depth = 0;
        for (size_t i = qpos + 1; i < s.size(); ++i) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') depth--;
            else if (s[i] == ':' && depth == 0) {
                return {{"op", "ternary"},
                        {"args", json::array({parse_expr_ast(s.substr(0, qpos)),
                                               parse_expr_ast(s.substr(qpos + 1, i - qpos - 1)),
                                               parse_expr_ast(s.substr(i + 1))})}};
            }
        }
    }

    const std::vector<std::pair<std::string, std::string>> ops = {
        {"||", "or"}, {"&&", "and"}, {"!=", "neq"}, {"==", "eq"},
        {">=", "ge"}, {"<=", "le"}, {">", "gt"}, {"<", "lt"},
        {"+", "add"}, {"-", "sub"}, {"*", "mul"}
    };
    for (const auto& item : ops) {
        int pos = find_top_level_op(s, item.first);
        if (pos > 0) {
            return {{"op", item.second},
                    {"args", json::array({parse_expr_ast(s.substr(0, pos)),
                                           parse_expr_ast(s.substr(pos + item.first.size()))})}};
        }
    }
    return parse_atom(s);
}

void collect_ast_signals(const json& node, std::set<std::string>& signals) {
    if (!node.is_object()) return;
    std::string kind = node.value("kind", node.value("type", ""));
    if (kind == "signal") {
        std::string name = node.value("name", "");
        if (!name.empty()) signals.insert(name);
    }
    if (node.contains("base_signal") && node["base_signal"].is_string()) {
        std::string name = node["base_signal"].get<std::string>();
        if (!name.empty()) signals.insert(name);
    }
    for (const auto& item : node.items()) {
        if (item.value().is_object()) collect_ast_signals(item.value(), signals);
        if (item.value().is_array()) {
            for (const auto& elem : item.value()) collect_ast_signals(elem, signals);
        }
    }
}

std::string expr_op(const json& expr) {
    if (!expr.is_object()) return "";
    return expr.value("op", "");
}

bool expr_mentions_signal(const json& expr, const std::string& signal) {
    std::set<std::string> signals;
    collect_ast_signals(expr, signals);
    std::string target_leaf = leaf_name(signal);
    for (const auto& name : signals) {
        if (name == signal || leaf_name(name) == target_leaf) return true;
    }
    return false;
}

json signal_array_from_ast(const json& expr) {
    std::set<std::string> signals;
    collect_ast_signals(expr, signals);
    json out = json::array();
    for (const auto& name : signals) out.push_back(name);
    return out;
}

std::string rhs_from_source(const std::string& source) {
    std::string s = trim(source);
    size_t eq = std::string::npos;
    size_t le = s.find("<=");
    if (le != std::string::npos) eq = le + 1;
    else eq = s.find('=');
    if (eq == std::string::npos) return "";
    std::string rhs = s.substr(eq + 1);
    size_t semi = rhs.rfind(';');
    if (semi != std::string::npos) rhs = rhs.substr(0, semi);
    return trim(rhs);
}

std::string assignment_kind_from_source(const std::string& source) {
    std::string s = trim(source);
    if (starts_with(s, "assign ")) return "continuous_assign";
    if (s.find("<=") != std::string::npos) return "clocked_update";
    if (s.find("=") != std::string::npos) return "procedural_assignment";
    return "statement_only";
}

std::string confidence_for_trace(const json& trace) {
    if (!trace.value("ok", true)) return "unknown";
    if (trace.value("has_statement_only", false)) return "low";
    int count = trace.value("result_count", 0);
    if (count <= 0 && trace.value("control_dependency_count", 0) <= 0) return "unknown";
    bool has_source = false;
    for (const auto& r : trace.value("results", json::array())) {
        if (!r.value("source", "").empty()) has_source = true;
    }
    if (has_source) return "high";
    return "medium";
}

json enrich_trace_payload(const json& request, const json& trace) {
    json out = trace;
    std::string query = trace.value("query", request.value("args", json::object()).value("signal", ""));
    std::string mode = trace.value("mode", "driver");

    json rhs_signals = json::array();
    json edges = json::array();
    json records = trace.value("results", json::array());
    for (const auto& r : records) {
        std::string signal = r.value("signal", "");
        std::string role = r.value("role", "");
        std::string resolution = r.value("resolution", "");
        if (!signal.empty() && resolution != "statement_only") rhs_signals.push_back(signal);

        std::string edge_type = resolution == "statement_only" ? "statement_only" : "data_dependency";
        if (role == "lhs_target") edge_type = "load_dependency";
        json edge = {
            {"from", mode == "driver" ? signal : query},
            {"to", mode == "driver" ? query : signal},
            {"type", edge_type},
            {"role", role},
            {"file", r.value("file", "")},
            {"line", r.value("line", 0)},
            {"source", r.value("source", "")},
            {"resolution", resolution},
            {"confidence", edge_type == "statement_only" ? "low" : "high"}
        };
        edges.push_back(edge);
    }

    for (const auto& r : trace.value("control_dependencies", json::array())) {
        std::string signal = r.value("signal", "");
        edges.push_back({
            {"from", signal},
            {"to", query},
            {"type", "control_dependency"},
            {"relation", "controls_assignment"},
            {"file", r.value("file", "")},
            {"line", r.value("line", 0)},
            {"source", r.value("source", "")},
            {"resolution", r.value("resolution", "")},
            {"confidence", "medium"}
        });
    }

    std::string confidence = confidence_for_trace(trace);
    out["rhs_signals"] = rhs_signals;
    out["dependency_edges"] = edges;
    out["confidence"] = confidence;
    out["confidence_reason"] =
        confidence == "high" ? "exact signal references with source locations were resolved" :
        confidence == "medium" ? "trace records were resolved but structured expression is incomplete" :
        confidence == "low" ? "trace contains statement_only or fallback records" :
        "no reliable trace result was resolved";

    if (!records.empty()) {
        std::string source = records[0].value("source", "");
        std::string rhs = rhs_from_source(source);
        if (!rhs.empty()) {
            out["assignment"] = {
                {"kind", assignment_kind_from_source(source)},
                {"lhs", {{"type", "signal"}, {"name", query}}},
                {"rhs", parse_expr_ast(rhs)},
                {"source", source}
            };
        }
    }
    return out;
}

json make_trace_summary(const json& trace) {
    return {
        {"query", trace.value("query", "")},
        {"mode", trace.value("mode", "")},
        {"result_count", trace.value("result_count", 0)},
        {"control_dependency_count", trace.value("control_dependency_count", 0)},
        {"truncated", trace.value("truncated", false)},
        {"confidence", trace.value("confidence", "unknown")}
    };
}

json run_trace_action(const json& request, const std::string& mode) {
    json response = base_response(request, request.value("action", ""));
    int session_id = -1;
    SessionInfo session;
    if (!ensure_target_session(request, response, session_id, session)) return response;

    json args = request.value("args", json::object());
    std::string signal = args.value("signal", args.value("root_signal", ""));
    if (signal.empty()) {
        return error_response(request, request.value("action", ""), "MISSING_FIELD", "args.signal is required");
    }

    std::string cmd = (mode == "load" ? CMD_LOAD_AI : CMD_DRIVER_AI);
    cmd += " " + signal + option_string_from_limits_args(request);

    json trace;
    std::string status, message;
    if (!send_json_command(session_id, cmd, trace, status, message)) {
        return error_response(request, request.value("action", ""), "SESSION_UNHEALTHY", message.empty() ? status : message);
    }

    json enriched = trace.contains("dependency_edges") ? trace : enrich_trace_payload(request, trace);
    response["ok"] = enriched.value("ok", true);
    response["summary"] = make_trace_summary(enriched);
    response["data"] = enriched;
    response["meta"]["truncated"] = enriched.value("truncated", false);
    if (!response["ok"].get<bool>()) {
        response["error"] = {
            {"code", "SIGNAL_NOT_FOUND"},
            {"message", enriched.value("error", "trace failed")},
            {"recoverable", true},
            {"candidates", json::array()},
            {"suggested_actions", json::array({{
                {"tool", "shell"},
                {"action", "rg"},
                {"reason", "xtrace requires an exact signal path; use source grep to discover candidate names"},
                {"args", {{"query", leaf_name(signal)}}}
            }})}
        };
    }
    return response;
}

json run_signal_resolve_action(const json& request) {
    json response = base_response(request, request.value("action", ""));
    int session_id = -1;
    SessionInfo session;
    if (!ensure_target_session(request, response, session_id, session)) return response;

    json args = request.value("args", json::object());
    std::string query = args.value("signal", args.value("query", ""));
    if (query.empty()) return error_response(request, request.value("action", ""), "MISSING_FIELD", "args.signal or args.query is required");

    std::string cmd = std::string(CMD_SIGNAL_RESOLVE) + " " + query;

    json payload;
    std::string status, message;
    if (!send_json_command(session_id, cmd, payload, status, message)) {
        return error_response(request, request.value("action", ""), "SESSION_UNHEALTHY", message.empty() ? status : message);
    }
    response["ok"] = payload.value("ok", true);
    response["summary"] = {
        {"query", payload.value("query", query)},
        {"count", payload.value("count", 0)},
        {"truncated", payload.value("truncated", false)}
    };
    response["data"] = payload;
    response["meta"]["truncated"] = payload.value("truncated", false);
    if (!response["ok"].get<bool>()) {
        response["error"] = {
            {"code", "SIGNAL_NOT_FOUND"},
            {"message", payload.value("message", "signal not found")},
            {"recoverable", true},
            {"candidates", payload.value("matches", json::array())},
            {"suggested_actions", json::array({{
                {"tool", "shell"},
                {"action", "rg"},
                {"reason", "xtrace signal.resolve only accepts exact paths; use source grep to discover candidate names"},
                {"args", {{"query", leaf_name(query)}}}
            }})}
        };
    }
    return response;
}

json canonicalize_signal(const json& request) {
    json resolved = run_signal_resolve_action(request);
    if (!resolved.value("ok", false)) return resolved;
    json data = resolved.value("data", json::object());
    std::string query = data.value("query", request.value("args", json::object()).value("signal", ""));
    std::string canonical = query;
    json matches = data.value("matches", json::array());
    if (!matches.empty()) canonical = matches[0].value("signal", query);
    bool ambiguous = matches.size() > 1;
    std::string base = canonical;
    std::string select;
    size_t bracket = canonical.find('[');
    if (bracket != std::string::npos) {
        base = canonical.substr(0, bracket);
        select = canonical.substr(bracket);
    }
    size_t dot = base.rfind('.');
    resolved["summary"]["canonical"] = canonical;
    resolved["summary"]["ambiguous"] = ambiguous;
    resolved["data"]["canonical"] = canonical;
    resolved["data"]["rtl_path"] = canonical;
    resolved["data"]["query"] = query;
    resolved["data"]["leaf"] = leaf_name(canonical);
    resolved["data"]["scope"] = dot == std::string::npos ? "" : base.substr(0, dot);
    resolved["data"]["base_signal"] = base;
    resolved["data"]["select"] = select;
    resolved["data"]["ambiguous"] = ambiguous;
    resolved["data"]["aliases"] = query == canonical ? json::array({canonical}) : json::array({query, canonical});
    resolved["data"]["fsdb_candidates"] = json::array({canonical});
    resolved["data"]["port_mappings"] = json::array();
    return resolved;
}

json graph_from_trace(const json& trace, const std::string& root) {
    json nodes = json::array();
    json edges = trace.value("dependency_edges", json::array());
    std::map<std::string, std::string> ids;
    auto add_node = [&](const std::string& signal, const std::string& role) {
        if (signal.empty() || ids.count(signal)) return;
        std::string id = "n" + std::to_string(ids.size());
        ids[signal] = id;
        nodes.push_back({{"id", id}, {"signal", signal}, {"kind", "signal"}, {"role", role}});
    };
    add_node(root, "root");
    for (const auto& e : edges) {
        add_node(e.value("from", ""), "dependency");
        add_node(e.value("to", ""), "dependency");
    }
    json graph_edges = json::array();
    for (auto e : edges) {
        std::string from = e.value("from", "");
        std::string to = e.value("to", "");
        e["from"] = ids.count(from) ? ids[from] : from;
        e["to"] = ids.count(to) ? ids[to] : to;
        e["from_signal"] = from;
        e["to_signal"] = to;
        graph_edges.push_back(e);
    }
    return {{"nodes", nodes}, {"edges", graph_edges}};
}

std::string confidence_for_edge(const json& edge) {
    std::string confidence = edge.value("confidence", "");
    if (!confidence.empty()) return confidence;
    std::string type = edge.value("type", "");
    std::string resolution = edge.value("resolution", "");
    if (type == "statement_only" || resolution == "statement_only") return "low";
    if (type == "control_dependency") return "medium";
    if (!edge.value("source", "").empty()) return "high";
    return "medium";
}

json explanation_from_edge(const json& edge,
                           const std::string& root,
                           const std::string& direction,
                           int& skipped_empty_dependency_count) {
    std::string from = edge.value("from", "");
    std::string to = edge.value("to", "");
    std::string type = edge.value("type", "dependency");
    std::string related = direction == "load" ? to : from;
    json related_signals = json::array();
    if (!related.empty()) related_signals.push_back(related);

    if (related.empty() && type != "statement_only") {
        skipped_empty_dependency_count++;
        return nullptr;
    }

    std::string claim;
    if (type == "control_dependency") {
        claim = root + " is controlled by " + related;
    } else if (type == "statement_only") {
        claim = root + " has assignment evidence without resolved dependencies";
    } else if (direction == "load") {
        claim = root + " can affect " + related;
    } else {
        claim = root + " depends on " + related;
    }

    return {
        {"claim", claim},
        {"evidence", json::array({{
            {"type", type},
            {"file", edge.value("file", "")},
            {"line", edge.value("line", 0)},
            {"source", edge.value("source", "")},
            {"resolution", edge.value("resolution", "")}
        }})},
        {"related_signals", related_signals},
        {"confidence", confidence_for_edge(edge)}
    };
}

bool edge_type_allowed(const json& args, const json& edge) {
    json types = args.value("dependency_types", json::array());
    if (types.empty()) return true;
    std::string edge_type = edge.value("type", "");
    std::string assignment_type = edge.value("assignment_type", "");
    for (const auto& t : types) {
        if (!t.is_string()) continue;
        std::string want = t.get<std::string>();
        if (edge_type == want || assignment_type == want) return true;
        if (want == "data" && (edge_type == "data_dependency" ||
                               edge_type == "continuous_assignment" ||
                               edge_type == "procedural_assignment")) return true;
        if (want == "control" && edge_type == "control_dependency") return true;
        if (want == "load" && edge_type == "load_dependency") return true;
    }
    return false;
}

json trace_expand_like(const json& request, bool explain_only = false) {
    json response = base_response(request, request.value("action", ""));
    int session_id = -1;
    SessionInfo session;
    if (!ensure_target_session(request, response, session_id, session)) return response;

    json args = request.value("args", json::object());
    std::string root = args.value("root_signal", args.value("signal", ""));
    std::string direction = args.value("direction", "driver");
    if (root.empty()) return error_response(request, request.value("action", ""), "MISSING_FIELD", "args.root_signal or args.signal is required");

    const json limits = request.value("limits", json::object());
    int max_depth = limits.value("max_depth", 1);
    int max_nodes = limits.value("max_nodes", 100);
    int max_edges = limits.value("max_edges", 200);
    if (max_depth < 1) max_depth = 1;
    if (max_nodes < 1) max_nodes = 1;
    if (max_edges < 1) max_edges = 1;

    json all_edges = json::array();
    json traces = json::array();
    std::set<std::string> visited;
    std::vector<std::pair<std::string, int>> queue;
    queue.push_back(std::make_pair(root, 0));
    bool truncated = false;
    int reached_depth = 0;

    for (size_t qi = 0; qi < queue.size(); ++qi) {
        std::string current = queue[qi].first;
        int depth = queue[qi].second;
        reached_depth = std::max(reached_depth, depth);
        if (visited.count(current)) continue;
        if ((int)visited.size() >= max_nodes) {
            truncated = true;
            break;
        }
        visited.insert(current);
        if (depth >= max_depth) continue;

        json trace_req = request;
        trace_req["action"] = direction == "load" ? "trace.load" : "trace.driver";
        trace_req["target"] = {{"session_id", session_id}};
        trace_req["args"]["signal"] = current;
        json trace_resp = run_trace_action(trace_req, direction == "load" ? "load" : "driver");
        if (!trace_resp.value("ok", false)) {
            continue;
        }
        json trace = trace_resp["data"];
        traces.push_back(trace);
        if (trace.value("truncated", false)) truncated = true;
        for (const auto& e : trace.value("dependency_edges", json::array())) {
            if (!edge_type_allowed(args, e)) continue;
            if ((int)all_edges.size() >= max_edges) {
                truncated = true;
                break;
            }
            all_edges.push_back(e);
            std::string next = direction == "load" ? e.value("to", "") : e.value("from", "");
            if (!next.empty() && !visited.count(next) && (int)queue.size() < max_nodes) {
                queue.push_back(std::make_pair(next, depth + 1));
            } else if ((int)queue.size() >= max_nodes) {
                truncated = true;
            }
        }
        if ((int)all_edges.size() >= max_edges) {
            truncated = true;
            break;
        }
    }

    json trace = {
        {"query", root},
        {"mode", direction},
        {"dependency_edges", all_edges},
        {"confidence", traces.empty() ? "unknown" : traces[0].value("confidence", "unknown")},
        {"truncated", truncated}
    };
    json graph = graph_from_trace(trace, root);
    response["summary"] = {
        {"root_signal", root},
        {"direction", direction},
        {"depth", reached_depth},
        {"node_count", graph["nodes"].size()},
        {"edge_count", graph["edges"].size()},
        {"truncated", truncated}
    };
    response["meta"]["truncated"] = truncated;
    if (explain_only) {
        json explanations = json::array();
        int skipped_empty_dependency_count = 0;
        for (const auto& e : trace.value("dependency_edges", json::array())) {
            json explanation = explanation_from_edge(e, root, direction, skipped_empty_dependency_count);
            if (!explanation.is_null()) explanations.push_back(explanation);
        }
        response["summary"]["explanation_count"] = explanations.size();
        response["summary"]["skipped_empty_dependency_count"] = skipped_empty_dependency_count;
        response["data"] = {{"explanations", explanations}, {"trace", trace}, {"traces", traces}};
        response["suggested_next_actions"] = json::array({{
            {"tool", "xwave"},
            {"action", "value.at"},
            {"reason", "verify dependency signal value at the observed waveform time"},
            {"args", {{"signal", root}}}
        }});
    } else {
        response["data"] = {{"graph", graph}, {"trace", trace}, {"traces", traces}};
    }
    return response;
}

json trace_path(const json& request) {
    json args = request.value("args", json::object());
    std::string from = args.value("from_signal", "");
    std::string to = args.value("to_signal", "");
    json expand_req = request;
    if (!args.contains("root_signal") && !args.contains("signal") && !to.empty()) {
        expand_req["args"]["root_signal"] = to;
    }
    json response = trace_expand_like(expand_req, false);
    if (!response.value("ok", false)) return response;
    int max_paths = request.value("limits", json::object()).value("max_paths", 10);
    if (max_paths <= 0) max_paths = 10;
    bool found = false;
    json paths = json::array();
    json edges = response["data"]["graph"].value("edges", json::array());
    if (from.empty() || to.empty()) {
        for (const auto& e : edges) {
            if ((from.empty() || e.value("from_signal", "") == from) &&
                (to.empty() || e.value("to_signal", "") == to)) {
                found = true;
                paths.push_back(json::array({e}));
                if ((int)paths.size() >= max_paths) break;
            }
        }
    } else {
        std::vector<json> edge_vec;
        for (const auto& e : edges) edge_vec.push_back(e);
        std::vector<std::pair<std::string, json>> queue;
        queue.push_back(std::make_pair(from, json::array()));
        std::set<std::string> visited;
        for (size_t qi = 0; qi < queue.size() && (int)paths.size() < max_paths; ++qi) {
            std::string current = queue[qi].first;
            json path = queue[qi].second;
            if (current == to) {
                found = true;
                paths.push_back(path);
                continue;
            }
            if (visited.count(current)) continue;
            visited.insert(current);
            for (const auto& e : edge_vec) {
                if (e.value("from_signal", "") != current) continue;
                std::string next = e.value("to_signal", "");
                if (next.empty()) continue;
                json next_path = path;
                next_path.push_back(e);
                queue.push_back(std::make_pair(next, next_path));
            }
        }
    }
    if (!found) {
        for (const auto& e : edges) {
            if ((from.empty() || e.value("from_signal", "") == from) &&
                (to.empty() || e.value("to_signal", "") == to)) {
            found = true;
            paths.push_back(json::array({e}));
                break;
            }
        }
    }
    response["summary"]["from_signal"] = from;
    response["summary"]["to_signal"] = to;
    response["summary"]["path_count"] = paths.size();
    response["summary"]["found"] = found;
    response["data"]["paths"] = paths;
    return response;
}

json infer_enclosing_block(const std::vector<std::string>& lines, int line) {
    json enclosing = {{"type", "unknown"}, {"name", ""}, {"begin_line", 1}, {"end_line", (int)lines.size()}};
    int best_line = 0;
    for (int i = std::min(line, (int)lines.size()); i >= 1; --i) {
        std::string text = trim(lines[i - 1]);
        std::string low = lower_copy(text);
        std::string type;
        std::string name;
        if (starts_with(low, "module ") || low.find(" module ") != std::string::npos) {
            type = "module";
            name = next_token_after(text, "module");
        } else if (low.find("always_ff") != std::string::npos) {
            type = "always_ff";
        } else if (low.find("always_comb") != std::string::npos) {
            type = "always_comb";
        } else if (low.find("always") != std::string::npos) {
            type = "always";
        } else if (low.find("case") != std::string::npos) {
            type = "case";
        } else if (low.find("if") != std::string::npos && low.find("(") != std::string::npos) {
            type = "if";
        } else if (low.find("begin") != std::string::npos) {
            type = "begin";
        }
        if (!type.empty()) {
            enclosing["type"] = type;
            enclosing["name"] = name;
            enclosing["begin_line"] = i;
            best_line = i;
            break;
        }
    }
    if (best_line > 0) {
        std::string end_token = enclosing["type"] == "module" ? "endmodule" :
                                enclosing["type"] == "case" ? "endcase" : "end";
        for (int i = line; i <= (int)lines.size(); ++i) {
            if (lower_copy(lines[i - 1]).find(end_token) != std::string::npos) {
                enclosing["end_line"] = i;
                break;
            }
        }
    }
    return enclosing;
}

json source_context(const json& request) {
    json response = base_response(request, request.value("action", ""));
    json args = request.value("args", json::object());
    std::string file = args.value("file", "");
    int line = args.value("line", 0);
    int context_lines = args.value("context_lines", 8);
    if (file.empty() || line <= 0) {
        return error_response(request, request.value("action", ""), "MISSING_FIELD", "args.file and args.line are required");
    }
    std::ifstream in(file.c_str());
    if (!in) return error_response(request, request.value("action", ""), "SOURCE_NOT_FOUND", "source file not found: " + file);
    std::vector<std::string> lines;
    std::string s;
    while (std::getline(in, s)) lines.push_back(s);
    if (line > static_cast<int>(lines.size())) return error_response(request, request.value("action", ""), "INVALID_REQUEST", "line is out of range");
    int begin = std::max(1, line - context_lines);
    int end = std::min(static_cast<int>(lines.size()), line + context_lines);
    json ctx = json::array();
    for (int i = begin; i <= end; ++i) {
        ctx.push_back({{"line", i}, {"text", lines[i - 1]}, {"hit", i == line}});
    }
    response["summary"] = {{"file", file}, {"line", line}};
    response["data"] = {
        {"context", ctx},
        {"enclosing", infer_enclosing_block(lines, line)}
    };
    return response;
}

json control_explain(const json& request) {
    json trace_req = request;
    trace_req["action"] = "trace.driver";
    json trace_resp = run_trace_action(trace_req, "driver");
    if (!trace_resp.value("ok", false)) return trace_resp;
    json deps = trace_resp["data"].value("control_dependencies", json::array());
    for (auto& dep : deps) {
        std::string source = dep.value("source", "");
        std::string cond = source;
        size_t if_pos = cond.find("if");
        size_t lparen = cond.find('(', if_pos == std::string::npos ? 0 : if_pos);
        size_t rparen = cond.rfind(')');
        if (lparen != std::string::npos && rparen != std::string::npos && rparen > lparen) {
            cond = cond.substr(lparen + 1, rparen - lparen - 1);
        }
        dep["condition_text"] = trim(cond);
        dep["condition"] = parse_expr_ast(cond);
        dep["condition_signals"] = json::array({dep.value("signal", "")});
        dep["confidence"] = dep.value("source", "").empty() ? "low" : "medium";
    }
    trace_resp["action"] = request.value("action", "control.explain");
    trace_resp["summary"] = {
        {"signal", request.value("args", json::object()).value("signal", "")},
        {"control_dependency_count", deps.size()}
    };
    trace_resp["data"] = {{"control_dependencies", deps}};
    return trace_resp;
}

json condition_from_control_dep(const json& dep) {
    std::string source = dep.value("source", "");
    std::string cond = dep.value("condition_text", "");
    if (cond.empty()) {
        cond = source;
        size_t lparen = cond.find('(');
        size_t rparen = cond.rfind(')');
        if (lparen != std::string::npos && rparen != std::string::npos && rparen > lparen) {
            cond = cond.substr(lparen + 1, rparen - lparen - 1);
        }
    }
    return {{"text", trim(cond)}, {"ast", dep.value("condition", parse_expr_ast(cond))},
            {"signals", dep.value("condition_signals", json::array())},
            {"file", dep.value("file", "")}, {"line", dep.value("line", 0)},
            {"source", source}, {"confidence", dep.value("confidence", "medium")}};
}

json infer_clock_reset_from_assignment(const json& assignment, const json& control_deps) {
    json out = {{"clock", nullptr}, {"reset", nullptr}, {"event_controls", json::array()}};
    std::string file = assignment.value("location", json::object()).value("file", "");
    int line = assignment.value("location", json::object()).value("line", 0);
    if (!file.empty() && line > 0) {
        std::ifstream in(file.c_str());
        std::vector<std::string> lines;
        std::string s;
        while (std::getline(in, s)) lines.push_back(s);
        int begin = std::max(1, line - 40);
        for (int i = line; i >= begin; --i) {
            std::string text = lines[i - 1];
            std::string low = lower_copy(text);
            if (low.find("@") == std::string::npos &&
                low.find("always_ff") == std::string::npos &&
                low.find("always ") == std::string::npos) {
                continue;
            }
            std::string pos = next_token_after(text, "posedge");
            std::string neg = next_token_after(text, "negedge");
            if (!pos.empty()) {
                out["clock"] = pos;
                out["event_controls"].push_back({{"edge", "posedge"}, {"signal", pos}, {"line", i}, {"source", trim(text)}});
            }
            if (!neg.empty()) {
                out["reset"] = neg;
                out["event_controls"].push_back({{"edge", "negedge"}, {"signal", neg}, {"line", i}, {"source", trim(text)}});
            }
            if (!pos.empty() || !neg.empty()) break;
        }
    }
    for (const auto& dep : control_deps) {
        std::string sig = dep.value("signal", "");
        if (out["reset"].is_null() && (contains_word_like(sig, "rst") || contains_word_like(sig, "reset"))) {
            out["reset"] = sig;
        }
    }
    return out;
}

std::string classify_update_rule(const json& assignment, const json& condition, const std::string& target) {
    std::string cond_text = lower_copy(condition.value("text", ""));
    std::string source = lower_copy(assignment.value("source", ""));
    json rhs = assignment.value("rhs", json::object());
    std::string op = expr_op(rhs);
    if (cond_text.find("rst") != std::string::npos || cond_text.find("reset") != std::string::npos ||
        source.find("rst") != std::string::npos || source.find("reset") != std::string::npos) {
        return "reset";
    }
    if ((op == "add" || source.find("+") != std::string::npos) && expr_mentions_signal(rhs, target)) return "increment";
    if ((op == "sub" || source.find("-") != std::string::npos) && expr_mentions_signal(rhs, target)) return "decrement";
    if (expr_mentions_signal(rhs, target) && signal_array_from_ast(rhs).size() == 1) return "hold";
    return "update";
}

json normalize_assignments_with_conditions(const json& trace_data) {
    json out = json::array();
    json assignments = trace_data.value("assignments", json::array());
    json controls = trace_data.value("control_dependencies", json::array());
    for (auto assignment : assignments) {
        json conditions = json::array();
        for (const auto& dep : controls) conditions.push_back(condition_from_control_dep(dep));
        assignment["active_conditions"] = conditions;
        assignment["rhs_signals"] = assignment.value("rhs_signals", signal_array_from_ast(assignment.value("rhs", json::object())));
        assignment["assignment_role"] = conditions.empty() ? "default_or_unconditional" : "branch_assignment";
        out.push_back(assignment);
    }
    return out;
}

json run_procedural_assignment_action(const json& request) {
    json trace_req = request;
    trace_req["action"] = "trace.driver";
    json trace_resp = run_trace_action(trace_req, "driver");
    if (!trace_resp.value("ok", false)) return trace_resp;
    json response = base_response(request, request.value("action", ""));
    response["session"] = trace_resp["session"];
    std::string signal = request.value("args", json::object()).value("signal", "");
    json trace_data = trace_resp.value("data", json::object());
    json assignments = normalize_assignments_with_conditions(trace_data);
    json defaults = json::array();
    json branches = json::array();
    for (const auto& a : assignments) {
        if (a.value("assignment_role", "") == "default_or_unconditional") defaults.push_back(a);
        else branches.push_back(a);
    }
    response["summary"] = {
        {"signal", signal},
        {"assignment_count", assignments.size()},
        {"branch_count", branches.size()},
        {"default_count", defaults.size()},
        {"confidence", trace_data.value("confidence", "unknown")}
    };
    response["data"] = {{"procedural_assignment", {
        {"target", signal},
        {"enclosing_block", assignments.empty() ? json{{"type", "unknown"}} : json{{"type", "procedural_or_continuous"}, {"location", assignments[0].value("location", json::object())}}},
        {"assignments", assignments},
        {"default_assignments", defaults},
        {"branch_assignments", branches},
        {"control_dependencies", trace_data.value("control_dependencies", json::array())},
        {"dependency_edges", trace_data.value("dependency_edges", json::array())},
        {"confidence", trace_data.value("confidence", "unknown")},
        {"confidence_reason", trace_data.value("confidence_reason", "")}
    }}};
    return response;
}

json run_sequential_update_action(const json& request) {
    json proc_resp = run_procedural_assignment_action(request);
    if (!proc_resp.value("ok", false)) return proc_resp;
    json response = base_response(request, request.value("action", ""));
    response["session"] = proc_resp["session"];
    std::string signal = request.value("args", json::object()).value("signal", "");
    json proc = proc_resp["data"].value("procedural_assignment", json::object());
    json assignments = proc.value("assignments", json::array());
    json controls = proc.value("control_dependencies", json::array());
    json timing = assignments.empty() ? json{{"clock", nullptr}, {"reset", nullptr}, {"event_controls", json::array()}} :
                                      infer_clock_reset_from_assignment(assignments[0], controls);
    json rules = json::array();
    for (const auto& assignment : assignments) {
        json conditions = assignment.value("active_conditions", json::array());
        if (conditions.empty()) conditions.push_back({{"text", ""}, {"ast", json::object()}, {"signals", json::array()}});
        for (const auto& condition : conditions) {
            std::string rule_kind = classify_update_rule(assignment, condition, signal);
            rules.push_back({
                {"kind", rule_kind},
                {"condition", condition},
                {"next_value", assignment.value("rhs", json::object())},
                {"next_value_text", assignment.value("rhs", json::object()).value("text", assignment.value("source", ""))},
                {"rhs_signals", assignment.value("rhs_signals", json::array())},
                {"source", assignment.value("source", "")},
                {"location", assignment.value("location", json::object())}
            });
        }
    }
    response["summary"] = {
        {"signal", signal},
        {"rule_count", rules.size()},
        {"clock", timing.value("clock", json(nullptr))},
        {"reset", timing.value("reset", json(nullptr))},
        {"confidence", proc.value("confidence", "unknown")}
    };
    response["data"] = {{"sequential_update", {
        {"target", signal},
        {"clock", timing.value("clock", json(nullptr))},
        {"reset", timing.value("reset", json(nullptr))},
        {"event_controls", timing.value("event_controls", json::array())},
        {"rules", rules},
        {"confidence", proc.value("confidence", "unknown")},
        {"confidence_reason", proc.value("confidence_reason", "")}
    }}};
    return response;
}

json run_fsm_explain_action(const json& request) {
    json seq_resp = run_sequential_update_action(request);
    if (!seq_resp.value("ok", false)) return seq_resp;
    json response = base_response(request, request.value("action", ""));
    response["session"] = seq_resp["session"];
    std::string signal = request.value("args", json::object()).value("signal", "");
    json seq = seq_resp["data"].value("sequential_update", json::object());
    json transitions = json::array();
    for (const auto& rule : seq.value("rules", json::array())) {
        std::string kind = rule.value("kind", "");
        if (kind == "reset" || kind == "update") {
            transitions.push_back({
                {"from", "current"},
                {"to", rule.value("next_value_text", "")},
                {"condition", rule.value("condition", json::object())},
                {"kind", kind == "reset" ? "reset_transition" : "transition"},
                {"source", rule.value("source", "")},
                {"location", rule.value("location", json::object())}
            });
        }
    }
    response["summary"] = {
        {"signal", signal},
        {"transition_count", transitions.size()},
        {"confidence", seq.value("confidence", "unknown")}
    };
    response["data"] = {{"fsm", {
        {"state_signal", signal},
        {"clock", seq.value("clock", json(nullptr))},
        {"reset", seq.value("reset", json(nullptr))},
        {"transitions", transitions},
        {"rules", seq.value("rules", json::array())},
        {"confidence", seq.value("confidence", "unknown")},
        {"confidence_reason", seq.value("confidence_reason", "")}
    }}};
    return response;
}

json run_counter_explain_action(const json& request) {
    json seq_resp = run_sequential_update_action(request);
    if (!seq_resp.value("ok", false)) return seq_resp;
    json response = base_response(request, request.value("action", ""));
    response["session"] = seq_resp["session"];
    std::string signal = request.value("args", json::object()).value("signal", "");
    json seq = seq_resp["data"].value("sequential_update", json::object());
    json counter_rules = json::array();
    for (const auto& rule : seq.value("rules", json::array())) {
        std::string kind = rule.value("kind", "");
        if (kind == "reset" || kind == "increment" || kind == "decrement" || kind == "hold" || kind == "update") {
            counter_rules.push_back(rule);
        }
    }
    bool is_counter_like = false;
    for (const auto& rule : counter_rules) {
        std::string kind = rule.value("kind", "");
        if (kind == "increment" || kind == "decrement") is_counter_like = true;
    }
    std::string confidence = is_counter_like ? seq.value("confidence", "medium") : "medium";
    response["summary"] = {
        {"signal", signal},
        {"counter_like", is_counter_like},
        {"rule_count", counter_rules.size()},
        {"confidence", confidence}
    };
    response["data"] = {{"counter", {
        {"signal", signal},
        {"clock", seq.value("clock", json(nullptr))},
        {"reset", seq.value("reset", json(nullptr))},
        {"rules", counter_rules},
        {"counter_like", is_counter_like},
        {"confidence", confidence},
        {"confidence_reason", is_counter_like ? "increment/decrement rule was identified from next-value expression" : "sequential rules were found but no increment/decrement pattern was proven"}
    }}};
    return response;
}

json run_port_like_action(const json& request, const std::string& action) {
    json response = base_response(request, action);
    int session_id = -1;
    SessionInfo session;
    if (!ensure_target_session(request, response, session_id, session)) return response;

    json args = request.value("args", json::object());
    std::string path = args.value("path", args.value("instance", args.value("signal", args.value("interface", ""))));
    if (path.empty()) {
        return error_response(request, action, "MISSING_FIELD", "args.path, args.instance, args.signal, or args.interface is required");
    }
    int limit = request.value("limits", json::object()).value("max_results", args.value("limit", 0));
    std::string cmd = action == "port.trace" ? CMD_PORT_TRACE_AI :
                      action == "instance.map" ? CMD_INSTANCE_MAP_AI : CMD_INTERFACE_RESOLVE_AI;
    cmd += " " + path;
    if (limit > 0) cmd += " --limit " + std::to_string(limit);

    json payload;
    std::string status, message;
    if (!send_json_command(session_id, cmd, payload, status, message)) {
        return error_response(request, action, "SESSION_UNHEALTHY", message.empty() ? status : message);
    }
    response["ok"] = payload.value("ok", true);
    response["summary"] = {
        {"query", payload.value("query", path)},
        {"port_count", payload.value("port_count", 0)},
        {"modport_port_count", payload.value("modport_port_count", 0)},
        {"truncated", payload.value("truncated", false)}
    };
    response["data"] = payload;
    response["meta"]["truncated"] = payload.value("truncated", false);
    if (!response["ok"].get<bool>()) {
        json err = payload.value("error", json::object());
        response["error"] = {
            {"code", err.value("code", "TRACE_NO_RESULT")},
            {"message", err.value("message", "port/interface query failed")},
            {"recoverable", true},
            {"candidates", json::array()},
            {"suggested_actions", json::array()}
        };
    }
    return response;
}

json schema_payload() {
    return {
        {"api_version", API_VERSION},
        {"request", {
            {"api_version", API_VERSION},
            {"request_id", "optional-id"},
            {"action", "trace.driver"},
            {"target", {{"dbdir", "/path/to/simv.daidir"}, {"session_id", nullptr}, {"auto_ensure", true}}},
            {"args", json::object()},
            {"limits", {{"max_results", 50}, {"max_depth", 1}, {"max_paths", 10}, {"timeout_ms", 5000}}},
            {"output", {{"verbosity", "compact"}, {"include_source", true}, {"include_control_dependencies", true}, {"include_expr", true}, {"include_graph", false}}}
        }},
        {"response", {
            {"ok", true},
            {"action", "trace.driver"},
            {"tool", {{"name", "xtrace"}, {"version", TOOL_VERSION}}},
            {"session", json::object()},
            {"summary", json::object()},
            {"data", json::object()},
            {"findings", json::array()},
            {"suggested_next_actions", json::array()},
            {"warnings", json::array()},
            {"error", nullptr},
            {"meta", json::object()}
        }}
    };
}

json actions_payload() {
    json implemented = json::array({
        "session.open", "session.ensure", "session.list", "session.doctor", "session.kill", "session.close",
        "trace.driver", "trace.load", "trace.query",
        "signal.resolve", "signal.canonicalize",
        "trace.expand", "trace.graph", "trace.path", "trace.explain",
        "control.explain", "source.context",
        "expr.normalize", "procedural.assignment", "sequential.update",
        "fsm.explain", "counter.explain",
        "port.trace", "instance.map", "interface.resolve",
        "batch"
    });
    json experimental = json::array();
    return {{"api_version", API_VERSION}, {"implemented", implemented}, {"experimental", experimental}};
}

json handle_request(const json& request);

json run_batch(const json& request) {
    json response = base_response(request, "batch");
    json args = request.value("args", json::object());
    json requests = args.value("requests", json::array());
    std::string mode = args.value("mode", "continue_on_error");
    json results = json::array();
    bool all_ok = true;
    for (auto child : requests) {
        if (!child.contains("api_version")) child["api_version"] = API_VERSION;
        json child_resp = handle_request(child);
        if (!child_resp.value("ok", false)) all_ok = false;
        results.push_back(child_resp);
        if (!child_resp.value("ok", false) && mode == "stop_on_error") break;
    }
    response["ok"] = all_ok;
    response["summary"] = {{"count", results.size()}, {"all_ok", all_ok}};
    response["data"] = {{"results", results}};
    return response;
}

json handle_session_action(const json& request, const std::string& action) {
    json response = base_response(request, action);
    SessionManager manager;
    if (action == "session.open" || action == "session.ensure") {
        std::vector<std::string> args = target_dbdir_args(request);
        if (args.empty()) return error_response(request, action, "INVALID_TARGET", "target.dbdir is required");
        SessionEnsureResult result = manager.ensure_session(args);
        if (!result.ok) return error_response(request, action, "SESSION_UNHEALTHY", result.message);
        response["session"] = session_to_json(result.info);
        response["session"]["reused"] = result.reused;
        response["session"]["healthy"] = true;
        response["summary"] = {{"session_id", result.session_id}, {"status", result.status}, {"reused", result.reused}};
        response["data"] = {{"session", response["session"]}};
        return response;
    }
    if (action == "session.list") {
        json sessions = json::array();
        for (const auto& s : manager.list_sessions()) sessions.push_back(session_to_json(s));
        response["summary"] = {{"count", sessions.size()}};
        response["data"] = {{"sessions", sessions}};
        return response;
    }
    if (action == "session.doctor") {
        int sid = request.value("target", json::object()).value("session_id", request.value("args", json::object()).value("session_id", 0));
        if (sid <= 0) return error_response(request, action, "MISSING_FIELD", "target.session_id or args.session_id is required");
        SessionHealth h = manager.diagnose_session(sid);
        response["ok"] = h.healthy;
        response["session"] = session_to_json(h.info);
        response["summary"] = {{"session_id", sid}, {"healthy", h.healthy}, {"status", session_health_status_name(h.status)}, {"message", h.message}};
        response["data"] = {{"health", response["summary"]}};
        if (!h.healthy) response["error"] = {{"code", "SESSION_UNHEALTHY"}, {"message", h.message}, {"recoverable", true}, {"candidates", json::array()}, {"suggested_actions", json::array()}};
        return response;
    }
    if (action == "session.kill" || action == "session.close") {
        json args = request.value("args", json::object());
        if (args.value("id", "") == "all") {
            bool ok = manager.kill_all_sessions();
            response["ok"] = ok;
            response["summary"] = {{"target", "all"}, {"killed", ok}};
            return response;
        }
        int sid = 0;
        json target = request.value("target", json::object());
        if (target.contains("session_id") && target["session_id"].is_number_integer()) {
            sid = target["session_id"].get<int>();
        } else if (args.contains("session_id") && args["session_id"].is_number_integer()) {
            sid = args["session_id"].get<int>();
        } else if (args.contains("id")) {
            if (args["id"].is_number_integer()) sid = args["id"].get<int>();
            else if (args["id"].is_string()) sid = atoi(args["id"].get<std::string>().c_str());
        }
        if (sid <= 0 && action == "session.close") {
            SessionInfo latest;
            if (manager.get_latest_session(latest)) sid = latest.session_id;
        }
        if (sid <= 0) return error_response(request, action, "MISSING_FIELD", "session id is required");
        bool ok = manager.kill_session(sid);
        response["ok"] = ok;
        response["summary"] = {{"session_id", sid}, {"killed", ok}};
        if (!ok) response["error"] = {{"code", "SESSION_NOT_FOUND"}, {"message", "failed to kill session"}, {"recoverable", true}, {"candidates", json::array()}, {"suggested_actions", json::array()}};
        return response;
    }
    return error_response(request, action, "UNKNOWN_ACTION", "unknown session action");
}

json handle_request(const json& request) {
    std::string action = request.value("action", "");
    if (request.value("api_version", std::string(API_VERSION)) != API_VERSION) {
        return error_response(request, action, "UNSUPPORTED_API_VERSION", "expected xtrace.ai.v1", false);
    }
    if (action.empty()) return error_response(request, action, "MISSING_FIELD", "action is required");

    if (action == "batch") return run_batch(request);
    if (starts_with(action, "session.")) return handle_session_action(request, action);
    if (action == "trace.driver") return run_trace_action(request, "driver");
    if (action == "trace.load") return run_trace_action(request, "load");
    if (action == "trace.query") {
        std::string mode = request.value("args", json::object()).value("mode", "driver");
        return run_trace_action(request, mode == "load" ? "load" : "driver");
    }
    if (action == "signal.resolve") return run_signal_resolve_action(request);
    if (action == "signal.canonicalize") return canonicalize_signal(request);
    if (action == "trace.expand" || action == "trace.graph") return trace_expand_like(request, false);
    if (action == "trace.path") return trace_path(request);
    if (action == "trace.explain") return trace_expand_like(request, true);
    if (action == "control.explain") return control_explain(request);
    if (action == "source.context") return source_context(request);
    if (action == "expr.normalize") {
        json response = base_response(request, action);
        json args = request.value("args", json::object());
        std::string signal = args.value("signal", "");
        if (!signal.empty()) {
            json trace_req = request;
            trace_req["action"] = "trace.driver";
            trace_req["args"]["signal"] = signal;
            json trace_resp = run_trace_action(trace_req, "driver");
            if (!trace_resp.value("ok", false)) return trace_resp;
            json assignment = trace_resp["data"].value("assignment", json::object());
            response["session"] = trace_resp["session"];
            response["summary"] = {{"signal", signal}, {"source", "npi_trace_assignment"}, {"confidence", trace_resp["data"].value("confidence", "unknown")}};
            response["data"] = {{"expr", assignment.value("rhs", json::object())},
                                {"assignment", assignment},
                                {"rhs_signals", assignment.value("rhs_signals", json::array())},
                                {"confidence", trace_resp["data"].value("confidence", "unknown")}};
            return response;
        }
        std::string expr = args.value("expr", "");
        if (expr.empty()) return error_response(request, action, "MISSING_FIELD", "args.expr or args.signal is required");
        response["summary"] = {{"expr", expr}, {"source", "string_fallback"}, {"confidence", "low"}};
        response["data"] = {{"expr", parse_expr_ast(expr)}, {"confidence", "low"}, {"confidence_reason", "parsed from raw string without NPI handle"}};
        return response;
    }
    if (action == "procedural.assignment") return run_procedural_assignment_action(request);
    if (action == "sequential.update") return run_sequential_update_action(request);
    if (action == "fsm.explain") return run_fsm_explain_action(request);
    if (action == "counter.explain") return run_counter_explain_action(request);
    if (action == "port.trace" || action == "instance.map" || action == "interface.resolve") return run_port_like_action(request, action);

    return error_response(request, action, "UNKNOWN_ACTION", "unknown action: " + action, true);
}

int print_json_and_return(const json& payload) {
    printf("%s\n", payload.dump(2).c_str());
    return payload.value("ok", true) ? 0 : 1;
}

} // namespace

int cmd_ai(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s ai <query|schema|actions>\n", argv[0]);
        return 1;
    }

    std::string subcmd = argv[2];
    if (subcmd == "schema") {
        printf("%s\n", schema_payload().dump(2).c_str());
        return 0;
    }
    if (subcmd == "actions") {
        printf("%s\n", actions_payload().dump(2).c_str());
        return 0;
    }
    if (subcmd != "query") {
        json req = {{"api_version", API_VERSION}, {"action", ""}};
        return print_json_and_return(error_response(req, "", "UNKNOWN_ACTION", "unknown ai subcommand: " + subcmd));
    }

    std::string input;
    if (argc >= 5 && std::string(argv[3]) == "--json") {
        input = argv[4];
    } else if (argc >= 4 && std::string(argv[3]) == "-") {
        input = read_stream(std::cin);
    } else if (argc >= 4) {
        input = read_file(argv[3]);
        if (input.empty()) {
            json req = {{"api_version", API_VERSION}, {"action", ""}};
            return print_json_and_return(error_response(req, "", "INVALID_REQUEST", "failed to read request file"));
        }
    } else {
        json req = {{"api_version", API_VERSION}, {"action", ""}};
        return print_json_and_return(error_response(req, "", "INVALID_REQUEST", "ai query requires a file, -, or --json"));
    }

    try {
        json request = json::parse(input);
        return print_json_and_return(handle_request(request));
    } catch (const std::exception& e) {
        json req = {{"api_version", API_VERSION}, {"action", ""}};
        return print_json_and_return(error_response(req, "", "INVALID_REQUEST", e.what()));
    }
}

} // namespace xtrace
