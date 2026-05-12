#pragma once

namespace xtrace {

// Server main function - called when --server flag is passed
// argv: [exe, --server, session_id, ...design_args...]
int server_main(int argc, char** argv);

} // namespace xtrace
