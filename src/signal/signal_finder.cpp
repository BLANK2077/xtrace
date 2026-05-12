#include "signal_finder.h"

#include "json.hpp"

#include <algorithm>
#include <sstream>

namespace xtrace {

using json = nlohmann::json;

SignalSearchResult SignalFinder::resolve(const std::string& query, int limit) const {
    SignalSearchResult result;
    result.query = query;

    npiHandle direct = npi_handle_by_name(query.c_str(), NULL);
    if (direct) {
        result.matches.push_back(make_match(direct));
        npi_release_handle(direct);
        return result;
    }

    result = search(query, limit);
    if (result.matches.empty()) {
        result.ok = false;
        result.status = "not_found";
        result.message = "Signal not found: " + query;
    }
    return result;
}

SignalSearchResult SignalFinder::search(const std::string& pattern, int limit) const {
    SignalSearchResult result;
    result.query = pattern;

    npiHandle top_iter = npi_iterate(npiModule, NULL);
    if (!top_iter) {
        result.ok = false;
        result.status = "no_design";
        result.message = "No module hierarchy available";
        return result;
    }

    npiHandle scope = NULL;
    while ((scope = npi_scan(top_iter)) != NULL) {
        collect_scope(scope, pattern, limit, result);
        npi_release_handle(scope);
        if (result.truncated) {
            break;
        }
    }

    if (result.matches.empty()) {
        result.ok = false;
        result.status = "not_found";
        result.message = "Signal not found: " + pattern;
    }
    return result;
}

void SignalFinder::collect_scope(npiHandle scope,
                                 const std::string& pattern,
                                 int limit,
                                 SignalSearchResult& result) const {
    collect_objects(scope, npiPorts, pattern, limit, result);
    collect_objects(scope, npiNet, pattern, limit, result);
    collect_objects(scope, npiReg, pattern, limit, result);
    collect_objects(scope, npiVariables, pattern, limit, result);
    collect_objects(scope, npiBitVar, pattern, limit, result);

    npiHandle child_iter = npi_iterate(npiInstance, scope);
    if (!child_iter) {
        child_iter = npi_iterate(npiModule, scope);
    }
    if (!child_iter) {
        return;
    }

    npiHandle child = NULL;
    while ((child = npi_scan(child_iter)) != NULL) {
        collect_scope(child, pattern, limit, result);
        npi_release_handle(child);
        if (result.truncated) {
            break;
        }
    }
}

void SignalFinder::collect_objects(npiHandle scope,
                                   int object_type,
                                   const std::string& pattern,
                                   int limit,
                                   SignalSearchResult& result) const {
    if (result.truncated) {
        return;
    }

    npiHandle iter = npi_iterate(object_type, scope);
    if (!iter) {
        return;
    }

    npiHandle hdl = NULL;
    while ((hdl = npi_scan(iter)) != NULL) {
        add_match(hdl, pattern, limit, result);
        npi_release_handle(hdl);
        if (result.truncated) {
            break;
        }
    }
}

bool SignalFinder::add_match(npiHandle hdl,
                             const std::string& pattern,
                             int limit,
                             SignalSearchResult& result) const {
    SignalMatch match = make_match(hdl);
    if (match.signal.empty() || !matches_pattern(match.signal, pattern)) {
        return false;
    }

    for (const auto& existing : result.matches) {
        if (existing.signal == match.signal) {
            return false;
        }
    }

    if (limit > 0 && (int)result.matches.size() >= limit) {
        result.truncated = true;
        return false;
    }

    result.matches.push_back(match);
    return true;
}

SignalMatch SignalFinder::make_match(npiHandle hdl) const {
    SignalMatch match;
    const char* full_name = npi_get_str(npiFullName, hdl);
    if (full_name) {
        match.signal = full_name;
    }
    match.type = type_name(npi_get(npiType, hdl));
    const char* file = npi_get_str(npiFile, hdl);
    if (!file) {
        file = npi_get_str(npiDefFile, hdl);
    }
    if (file) {
        match.file = file;
    }
    match.line = npi_get(npiLineNo, hdl);
    if (match.line <= 0) {
        match.line = npi_get(npiDefLineNo, hdl);
    }
    return match;
}

bool SignalFinder::matches_pattern(const std::string& signal, const std::string& pattern) const {
    if (pattern.empty()) {
        return true;
    }
    if (signal == pattern) {
        return true;
    }
    if (signal.find(pattern) != std::string::npos) {
        return true;
    }
    size_t dot = signal.rfind('.');
    std::string leaf = dot == std::string::npos ? signal : signal.substr(dot + 1);
    return leaf == pattern;
}

std::string SignalFinder::type_name(int type) const {
    switch (type) {
        case npiPort: return "port";
        case npiNet: return "net";
        case npiReg: return "reg";
        case npiBitVar: return "bitvar";
        default: {
            std::ostringstream out;
            out << "type_" << type;
            return out.str();
        }
    }
}

std::string SignalFinder::render_text(const SignalSearchResult& result) const {
    std::ostringstream out;
    if (!result.ok) {
        out << "Error: " << result.message << " (status=" << result.status << ")\n";
        return out.str();
    }
    out << "Signal matches for " << result.query << "\n";
    int idx = 1;
    for (const auto& match : result.matches) {
        out << "[" << idx++ << "] " << match.signal << "\n";
        out << "    type: " << match.type << "\n";
        if (!match.file.empty() || match.line > 0) {
            out << "    location: " << match.file << ":" << match.line << "\n";
        }
    }
    if (result.truncated) {
        out << "(truncated)\n";
    }
    return out.str();
}

std::string SignalFinder::render_json(const SignalSearchResult& result) const {
    json payload;
    payload["ok"] = result.ok;
    payload["query"] = result.query;
    payload["status"] = result.status;
    payload["message"] = result.message;
    payload["count"] = result.matches.size();
    payload["truncated"] = result.truncated;
    payload["matches"] = json::array();
    for (const auto& match : result.matches) {
        payload["matches"].push_back({
            {"signal", match.signal},
            {"type", match.type},
            {"file", match.file},
            {"line", match.line}
        });
    }
    return payload.dump(2) + "\n";
}

} // namespace xtrace
