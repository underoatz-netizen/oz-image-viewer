// oz/viewer_core/marker_emit.hpp - Stage 0 timing-marker emission for the
// Stage 1 viewer, compatible with the ozbench runner's "markers" collector.
#pragma once

#include "oz/markers/markers.hpp"

#include <cstdint>
#include <mutex>

namespace oz::viewer_core {

// Emits Stage 0 timing markers as UTF-8 JSON Lines on stdout, in the exact
// format the ozbench runner parses:
//   {"marker":"ProcessStart","raw_ticks":123,"relative_ns":0,"thread_id":1234}
//
// The marker session is always constructed (recording ProcessStart as the
// first entry action). When disabled (normal mode) nothing is written to
// stdout; when enabled (--benchmark) each recorded marker is emitted
// immediately. record() is thread-safe.
class marker_emitter {
public:
    explicit marker_emitter(bool enabled);

    void record(oz::markers::marker_id id);

    const oz::markers::marker_session& session() const noexcept { return session_; }

private:
    void emit(const oz::markers::marker_record& r);

    bool enabled_;
    oz::markers::marker_session session_;
    mutable std::mutex mutex_;
};

}  // namespace oz::viewer_core