#pragma once

#include <string>
#include <vector>

#include "npi_hdl.h"

namespace xtrace {

enum class TraceMode {
    Driver,
    Load
};

struct TraceRecord {
    std::string signal;
    std::string role;
    std::string file;
    int line;
    std::string source;
    std::string resolution;
};

struct TraceResult {
    std::string query;
    std::string mode;
    std::vector<TraceRecord> results;
    std::vector<TraceRecord> control_dependencies;
    std::string error;
};

class TraceEngine {
public:
    TraceResult trace(const std::string& signal, TraceMode mode);

    std::string render_text(const TraceResult& result) const;
    std::string render_json(const TraceResult& result) const;

private:
    TraceResult trace_driver(const std::string& signal);
    TraceResult trace_load(const std::string& signal);

    void extract_expr_signals(npiHandle expr, std::vector<std::string>& signals) const;
    void extract_condition_deps(npiHandle condition,
                                npiHandle stmt,
                                std::vector<TraceRecord>& deps) const;
    TraceRecord make_record(const std::string& signal,
                            const std::string& role,
                            npiHandle stmt,
                            const std::string& resolution) const;
    TraceRecord make_statement_record(npiHandle stmt,
                                      const std::string& role,
                                      const std::string& resolution) const;
    std::string normalize_signal(npiHandle hdl) const;
    std::string source_line(const std::string& file, int line) const;
    bool should_skip_signal_name(const std::string& name) const;
    void add_unique(std::vector<TraceRecord>& records, const TraceRecord& record) const;
};

} // namespace xtrace
