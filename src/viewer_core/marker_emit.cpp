#include "oz/viewer_core/marker_emit.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>

namespace oz::viewer_core {

marker_emitter::marker_emitter(bool enabled) : enabled_(enabled) {
    if (enabled_) {
        emit(session_.records().back());  // ProcessStart
    }
}

void marker_emitter::record(oz::markers::marker_id id) {
    session_.record(id);
    if (enabled_) {
        emit(session_.records().back());
    }
}

void marker_emitter::emit(const oz::markers::marker_record& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Write directly to the inherited stdout handle (the runner sets up a pipe
    // via STARTF_USESTDHANDLES). A GUI-subsystem app has no CRT console, so we
    // avoid stdio and use WriteFile on the handle.
    const std::string line =
        "{\"marker\":\"" + std::string(oz::markers::marker_name(r.id)) +
        "\",\"raw_ticks\":" + std::to_string(r.raw_ticks) +
        ",\"relative_ns\":" + std::to_string(r.relative_ns) +
        ",\"thread_id\":" + std::to_string(r.thread_id) + "}\n";
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

}  // namespace oz::viewer_core