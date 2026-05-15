#include "port_analyzer.h"

#include "../ast/ast_extractor.h"
#include "../trace/trace_engine.h"
#include "json.hpp"

#include "npi_hdl.h"

namespace xtrace {

using json = nlohmann::json;

namespace {

std::string direction_name(int dir) {
    switch (dir) {
        case npiInput: return "input";
        case npiOutput: return "output";
        case npiInout: return "inout";
        case npiRef: return "ref";
        case npiNoDirection: return "none";
        default: return "direction_" + std::to_string(dir);
    }
}

std::string type_name(int type) {
    switch (type) {
        case npiModule: return "module";
        case npiPort: return "port";
        case npiInterface: return "interface";
        case npiInterfaceArray: return "interface_array";
        case npiModport: return "modport";
        case npiMpPort: return "modport_port";
        case npiNet: return "net";
        case npiReg: return "reg";
        case npiBitVar: return "bit_var";
        case npiRefObj: return "ref_obj";
        default: return "type_" + std::to_string(type);
    }
}

json location_json(npiHandle hdl) {
    json loc;
    const char* file = hdl ? npi_get_str(npiFile, hdl) : nullptr;
    int line = hdl ? npi_get(npiLineNo, hdl) : 0;
    loc["file"] = file ? file : "";
    loc["line"] = line;
    return loc;
}

json handle_json(npiHandle hdl, const AstExtractor& ast) {
    if (!hdl) return nullptr;
    json out;
    int type = npi_get(npiType, hdl);
    const char* name = npi_get_str(npiName, hdl);
    const char* full = npi_get_str(npiFullName, hdl);
    out["type"] = type_name(type);
    out["npi_type"] = type;
    out["name"] = name ? name : "";
    out["full_name"] = full ? full : "";
    out["text"] = ast.decompile(hdl);
    out["location"] = location_json(hdl);
    return out;
}

json connection_json(npiHandle port, const AstExtractor& ast) {
    json out;
    out["port"] = handle_json(port, ast);
    out["direction"] = direction_name(npi_get(npiDirection, port));
    npiHandle high = npi_handle(npiHighConn, port);
    out["highconn"] = handle_json(high, ast);
    if (high) npi_release_handle(high);
    npiHandle low = npi_handle(npiLowConn, port);
    out["lowconn"] = handle_json(low, ast);
    if (low) npi_release_handle(low);
    npiHandle actual = npi_handle(npiActual, port);
    out["actual"] = handle_json(actual, ast);
    if (actual) npi_release_handle(actual);
    npiHandle ref = npi_handle(npiRefObj, port);
    out["ref"] = handle_json(ref, ast);
    if (ref) npi_release_handle(ref);
    return out;
}

json collect_ports(npiHandle scope, const AstExtractor& ast) {
    json ports = json::array();
    if (!scope) return ports;
    npiHandle iter = npi_iterate(npiPort, scope);
    if (!iter) iter = npi_iterate(npiPorts, scope);
    if (!iter) return ports;
    npiHandle port;
    while ((port = npi_scan(iter)) != NULL) {
        ports.push_back(connection_json(port, ast));
        npi_release_handle(port);
    }
    npi_release_handle(iter);
    return ports;
}

json collect_mpports(npiHandle scope, const AstExtractor& ast) {
    json ports = json::array();
    if (!scope) return ports;
    npiHandle iter = npi_iterate(npiMpPort, scope);
    if (!iter) return ports;
    npiHandle port;
    while ((port = npi_scan(iter)) != NULL) {
        ports.push_back(connection_json(port, ast));
        npi_release_handle(port);
    }
    npi_release_handle(iter);
    return ports;
}

} // namespace

std::string PortAnalyzer::render_error(const std::string& code, const std::string& message) const {
    json payload;
    payload["ok"] = false;
    payload["error"] = {{"code", code}, {"message", message}};
    return payload.dump(2) + "\n";
}

std::string PortAnalyzer::render_instance_map(const std::string& instance_path) const {
    AstExtractor ast;
    npiHandle inst = npi_handle_by_name(instance_path.c_str(), NULL);
    if (!inst) return render_error("INSTANCE_NOT_FOUND", "instance not found: " + instance_path);
    json payload;
    payload["ok"] = true;
    payload["query"] = instance_path;
    payload["instance"] = handle_json(inst, ast);
    payload["ports"] = collect_ports(inst, ast);
    payload["port_count"] = payload["ports"].size();
    npi_release_handle(inst);
    return payload.dump(2) + "\n";
}

std::string PortAnalyzer::render_interface_resolve(const std::string& path) const {
    AstExtractor ast;
    npiHandle hdl = npi_handle_by_name(path.c_str(), NULL);
    if (!hdl) return render_error("INTERFACE_NOT_FOUND", "interface or member not found: " + path);
    json payload;
    payload["ok"] = true;
    payload["query"] = path;
    payload["object"] = handle_json(hdl, ast);
    payload["ports"] = collect_ports(hdl, ast);
    payload["modport_ports"] = collect_mpports(hdl, ast);
    payload["port_count"] = payload["ports"].size();
    payload["modport_port_count"] = payload["modport_ports"].size();
    npi_release_handle(hdl);
    return payload.dump(2) + "\n";
}

std::string PortAnalyzer::render_port_trace(const std::string& path, int limit) const {
    AstExtractor ast;
    npiHandle hdl = npi_handle_by_name(path.c_str(), NULL);
    if (!hdl) return render_error("PORT_OR_INSTANCE_NOT_FOUND", "port or instance not found: " + path);

    json payload;
    payload["ok"] = true;
    payload["query"] = path;
    payload["object"] = handle_json(hdl, ast);
    payload["ports"] = json::array();

    json ports;
    int type = npi_get(npiType, hdl);
    if (type == npiPort || type == npiMpPort) {
        ports.push_back(connection_json(hdl, ast));
    } else {
        ports = collect_ports(hdl, ast);
    }

    TraceEngine engine;
    int count = 0;
    for (auto port : ports) {
        if (limit > 0 && count >= limit) {
            payload["truncated"] = true;
            break;
        }
        std::string signal = port["port"].value("full_name", "");
        if (signal.empty()) signal = port["highconn"].is_object() ? port["highconn"].value("full_name", "") : "";
        if (!signal.empty()) {
            TraceOptions opt;
            opt.limit = 10;
            TraceResult tr = engine.trace(signal, TraceMode::Driver, opt);
            try {
                port["trace"] = json::parse(engine.render_ai_json(tr));
            } catch (...) {
                port["trace"] = json::object();
            }
        }
        payload["ports"].push_back(port);
        count++;
    }
    payload["port_count"] = payload["ports"].size();
    npi_release_handle(hdl);
    return payload.dump(2) + "\n";
}

} // namespace xtrace
