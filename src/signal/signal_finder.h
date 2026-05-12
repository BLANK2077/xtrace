#pragma once

#include <string>
#include <vector>

#include "npi_hdl.h"

namespace xtrace {

struct SignalMatch {
    std::string signal;
    std::string type;
    std::string file;
    int line = 0;
};

struct SignalSearchResult {
    bool ok = true;
    std::string status = "ok";
    std::string message;
    std::string query;
    std::vector<SignalMatch> matches;
    bool truncated = false;
};

class SignalFinder {
public:
    SignalSearchResult resolve(const std::string& query, int limit = 20) const;
    SignalSearchResult search(const std::string& pattern, int limit = 20) const;

    std::string render_text(const SignalSearchResult& result) const;
    std::string render_json(const SignalSearchResult& result) const;

private:
    void collect_scope(npiHandle scope,
                       const std::string& pattern,
                       int limit,
                       SignalSearchResult& result) const;
    void collect_objects(npiHandle scope,
                         int object_type,
                         const std::string& pattern,
                         int limit,
                         SignalSearchResult& result) const;
    bool add_match(npiHandle hdl,
                   const std::string& pattern,
                   int limit,
                   SignalSearchResult& result) const;
    SignalMatch make_match(npiHandle hdl) const;
    bool matches_pattern(const std::string& signal, const std::string& pattern) const;
    std::string type_name(int type) const;
};

} // namespace xtrace
