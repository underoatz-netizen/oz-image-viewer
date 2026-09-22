// oz/runner/process.hpp - direct CreateProcessW launcher (never cmd.exe).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oz::runner {

struct process_options {
    std::wstring executable;
    std::vector<std::wstring> args;
    std::optional<std::wstring> working_directory;
    std::uint32_t timeout_ms = 30000;
    bool capture_stdout = false;
};

struct process_result {
    bool launched = false;
    bool timed_out = false;
    std::uint32_t exit_code = 0;
    std::uint32_t error = 0;
    std::string error_message;
    std::string stdout_text;  // UTF-8 bytes captured from the child's stdout
    std::uint64_t duration_ns = 0;
    std::uint32_t pid = 0;  // child process id when launched
};

// Launches the executable directly with CreateProcessW. Arguments are quoted
// with the CommandLineToArgvW-compatible rule, so Unicode paths and spaces
// are safe. When capture_stdout is set, the child's stdout/stderr are piped
// back and read until EOF. On timeout the child is terminated.
process_result launch_process(const process_options& opts);

}  // namespace oz::runner