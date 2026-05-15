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

struct SignalResolveResult {
    bool ok = true;
    std::string status = "ok";
    std::string message;
    std::string query;
    std::vector<SignalMatch> matches;
    bool truncated = false;
};

class SignalFinder {
public:
    SignalResolveResult resolve(const std::string& query) const;

    std::string render_text(const SignalResolveResult& result) const;
    std::string render_json(const SignalResolveResult& result) const;

private:
    SignalMatch make_match(npiHandle hdl) const;
    std::string type_name(int type) const;
};

} // namespace xtrace
