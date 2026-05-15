#pragma once

#include <string>

namespace xtrace {

class PortAnalyzer {
public:
    std::string render_instance_map(const std::string& instance_path) const;
    std::string render_interface_resolve(const std::string& path) const;
    std::string render_port_trace(const std::string& path, int limit) const;

private:
    std::string render_error(const std::string& code, const std::string& message) const;
};

} // namespace xtrace
