// ozbench_fixture - synthetic benchmark subject used only by focused runner
// tests. It emits the twelve canonical markers as UTF-8 JSON Lines on stdout
// with a configurable delay, and can inject validation defects on request.
//
// Flags:
//   --delay-ms N        delay between markers (default 5)
//   --sleep-ms N        extra sleep before exit (default 0)
//   --exit-code N       process exit code (default 0)
//   --image-path PATH   accepted and ignored (mirrors real subject CLI)
//   --duplicate         record DecodeStart twice
//   --out-of-order      record DecodeComplete after FirstPresent
//   --missing           skip HeaderParsed
//   --unknown-marker    emit an extra line with an unknown marker name
//   --no-markers        emit nothing
#include "oz/markers/markers.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

void emit(const oz::markers::marker_record& r) {
    std::printf("{\"marker\":\"%s\",\"raw_ticks\":%llu,\"relative_ns\":%lld,\"thread_id\":%u}\n",
                std::string(oz::markers::marker_name(r.id)).c_str(),
                static_cast<unsigned long long>(r.raw_ticks),
                static_cast<long long>(r.relative_ns), r.thread_id);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    long delay_ms = 5;
    long sleep_ms = 0;
    int exit_code = 0;
    bool duplicate = false;
    bool out_of_order = false;
    bool missing = false;
    bool unknown = false;
    bool no_markers = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--delay-ms" && i + 1 < argc) {
            delay_ms = _wtoi(argv[++i]);
        } else if (a == L"--sleep-ms" && i + 1 < argc) {
            sleep_ms = _wtoi(argv[++i]);
        } else if (a == L"--exit-code" && i + 1 < argc) {
            exit_code = _wtoi(argv[++i]);
        } else if (a == L"--image-path" && i + 1 < argc) {
            ++i;  // accepted and ignored
        } else if (a == L"--duplicate") {
            duplicate = true;
        } else if (a == L"--out-of-order") {
            out_of_order = true;
        } else if (a == L"--missing") {
            missing = true;
        } else if (a == L"--unknown-marker") {
            unknown = true;
        } else if (a == L"--no-markers") {
            no_markers = true;
        } else {
            std::fwprintf(stderr, L"ozbench_fixture: unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    oz::markers::marker_session session;  // records ProcessStart
    if (!no_markers) {
        emit(session.records().back());
        Sleep(static_cast<DWORD>(delay_ms));

        const oz::markers::marker_id order[] = {
            oz::markers::marker_id::renderer_ready,
            oz::markers::marker_id::file_opened,
            oz::markers::marker_id::codec_detected,
            oz::markers::marker_id::header_parsed,
            oz::markers::marker_id::decode_start,
            oz::markers::marker_id::decode_complete,
            oz::markers::marker_id::gpu_upload_start,
            oz::markers::marker_id::gpu_upload_complete,
            oz::markers::marker_id::first_present,
            oz::markers::marker_id::folder_enumeration_start,
            oz::markers::marker_id::folder_enumeration_complete,
        };
        for (const auto id : order) {
            if (missing && id == oz::markers::marker_id::header_parsed) {
                continue;
            }
            if (out_of_order && id == oz::markers::marker_id::decode_complete) {
                continue;  // emitted later, after FirstPresent
            }
            session.record(id);
            emit(session.records().back());
            Sleep(static_cast<DWORD>(delay_ms));
        }
        if (out_of_order) {
            session.record(oz::markers::marker_id::decode_complete);
            emit(session.records().back());
            Sleep(static_cast<DWORD>(delay_ms));
        }
        if (duplicate) {
            session.record(oz::markers::marker_id::decode_start);
            emit(session.records().back());
        }
    }
    if (unknown) {
        std::printf("{\"marker\":\"BogusMarker\",\"raw_ticks\":0,\"relative_ns\":0,\"thread_id\":0}\n");
    }
    if (sleep_ms > 0) {
        Sleep(static_cast<DWORD>(sleep_ms));
    }
    return exit_code;
}