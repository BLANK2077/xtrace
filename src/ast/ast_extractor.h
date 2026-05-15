#pragma once

#include "json.hpp"
#include "npi_hdl.h"

#include <string>
#include <vector>

namespace xtrace {

class AstExtractor {
public:
    nlohmann::json expr_to_json(npiHandle hdl) const;
    nlohmann::json assignment_to_json(npiHandle stmt, const std::string& target) const;
    nlohmann::json source_location(npiHandle hdl) const;
    std::vector<std::string> collect_signal_names(npiHandle expr) const;
    std::string normalize_signal(npiHandle hdl) const;
    std::string decompile(npiHandle hdl) const;

private:
    bool is_signal_handle(npiHandle hdl) const;
    bool is_select_handle(npiHandle hdl) const;
    std::string op_name(int op_type) const;
    nlohmann::json operands_to_json(npiHandle hdl) const;
    nlohmann::json select_to_json(npiHandle hdl, const std::string& kind) const;
};

} // namespace xtrace
