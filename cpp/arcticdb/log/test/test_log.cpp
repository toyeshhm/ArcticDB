/* Copyright 2026 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software
 * will be governed by the Apache License, version 2.0.
 */

#include <arcticdb/log/log.hpp>

#include <logger.pb.h>

#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include <arcticdb/util/format_bytes.hpp>

TEST(TestLog, SmokeTest) { arcticdb::log::root().info("Some msg"); }

TEST(TestLog, ConfigureSingleton) {
    std::string txt_conf = R"pb(
sink_by_id {
    key: "console"
    value {
        console {
            has_color: true
            std_err: true
        }
    }
}
logger_by_id {
    key: "root"
    value {
        pattern: "*** [%H:%M:%S %z] [thread %t] %v ***"
        sink_ids: "console"
    }
}
    )pb";
    arcticdb::proto::logger::LoggersConfig cfg;
    google::protobuf::TextFormat::ParseFromString(txt_conf, &cfg);
    arcticdb::log::Loggers::instance().configure(cfg);
    arcticdb::log::root().info("Some msg");
}

TEST(TestLog, TestFormatBytes) {
    auto s = arcticdb::format_bytes(12345678);
    ASSERT_EQ(s, "12.35MB");
}

#include <arcticdb/log/console_sink.hpp>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

#ifdef _WIN32
// The fd-table calls pytest's capture makes, from the CRT it makes them through: Python's ucrtbase.dll, which this
// test binary also has in the process (python3X.dll imports it). Without it - a genuinely static executable - the
// binary's own CRT, which is then the one the sink consults too, so the test still exercises the path production
// takes in that binary.
struct CrtIo {
    int(__cdecl* dup)(int);
    int(__cdecl* dup2)(int, int);
    int(__cdecl* close)(int);
    int(__cdecl* open_osfhandle)(intptr_t, int);
    bool shared;
};

CrtIo crt_io() {
    for (const wchar_t* name : {L"ucrtbase.dll", L"ucrtbased.dll"}) {
        HMODULE module = ::GetModuleHandleW(name);
        if (module == nullptr)
            continue;
        CrtIo io{
                reinterpret_cast<int(__cdecl*)(int)>(::GetProcAddress(module, "_dup")),
                reinterpret_cast<int(__cdecl*)(int, int)>(::GetProcAddress(module, "_dup2")),
                reinterpret_cast<int(__cdecl*)(int)>(::GetProcAddress(module, "_close")),
                reinterpret_cast<int(__cdecl*)(intptr_t, int)>(::GetProcAddress(module, "_open_osfhandle")),
                true
        };
        if (io.dup != nullptr && io.dup2 != nullptr && io.close != nullptr && io.open_osfhandle != nullptr)
            return io;
    }
    return CrtIo{&::_dup, &::_dup2, &::_close, &::_open_osfhandle, false};
}

HANDLE create_file(const std::filesystem::path& path) {
    return ::CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
    );
}
#endif

} // namespace

// Regression test: a console sink created before fd 2 is redirected (as pytest's capture does) must write to the
// redirected target, not to whatever the original stderr handle was. With spdlog's stderr_sink_mt on Windows the line
// went to the cached HANDLE, whose value had by then been reused by another file.
//
// On Windows the redirect is made through the CRT pytest makes it through (see CrtIo), and a decoy file is opened
// immediately after it: the dup2 closes the HANDLE fd 2 held, the static CRT's fd 2 still names that value, and the
// decoy is the next allocation, so it is the first candidate to be given the value - the position LMDB's data.mdb was
// in under pytest. Whether it actually gets it is the kernel's choice (recorded as a test property); either way, the
// decoy must stay empty. Off Windows a dup2 leaves nothing stale in any table and the decoy is a plain bystander.
TEST(TestLog, ConsoleSinkFollowsStderrRedirection) {
    auto sink = arcticdb::log::make_console_sink(true, false);
    spdlog::logger logger("redirect_test", sink);
    logger.set_pattern("%v");

    const auto capture_path = std::filesystem::temp_directory_path() / "arcticdb_test_log_stderr_capture.txt";
    const auto decoy_path = std::filesystem::temp_directory_path() / "arcticdb_test_log_stderr_decoy.txt";
    std::filesystem::remove(capture_path);
    std::filesystem::remove(decoy_path);
    std::fflush(stderr);

#ifdef _WIN32
    const CrtIo io = crt_io();
    RecordProperty("redirected_through", io.shared ? "ucrtbase" : "static crt");

    // Both tables name the same HANDLE for fd 2. Save fd 2 in each before anything closes it, so each can be
    // restored through its own CRT at the end.
    const HANDLE original_handle = reinterpret_cast<HANDLE>(::_get_osfhandle(2));
    const int saved_static = ::_dup(2);
    ASSERT_GE(saved_static, 0);
    const int saved = io.dup(2);
    ASSERT_GE(saved, 0);

    const HANDLE capture_handle = create_file(capture_path);
    ASSERT_NE(capture_handle, INVALID_HANDLE_VALUE);
    const int capture_fd = io.open_osfhandle(reinterpret_cast<intptr_t>(capture_handle), 0);
    ASSERT_GE(capture_fd, 0);
    ASSERT_EQ(io.dup2(capture_fd, 2), 0);
    const HANDLE decoy_handle = create_file(decoy_path);
    ASSERT_NE(decoy_handle, INVALID_HANDLE_VALUE);
    const bool decoy_took_stale_handle = io.shared && decoy_handle == original_handle;
    RecordProperty("decoy_took_stale_stderr_handle", decoy_took_stale_handle ? "yes" : "no");
    ASSERT_EQ(io.close(capture_fd), 0);
#else
    const int saved = ::dup(2);
    ASSERT_GE(saved, 0);
    const int capture_fd = ::open(capture_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(capture_fd, 0);
    ASSERT_EQ(::dup2(capture_fd, 2), 2);
    const int decoy_fd = ::open(decoy_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(decoy_fd, 0);
    ASSERT_EQ(::close(capture_fd), 0);
#endif

    logger.warn("captured-line-42");
    logger.flush();
    std::fflush(stderr);

#ifdef _WIN32
    ASSERT_EQ(io.dup2(saved, 2), 0);
    io.close(saved);
    // The static CRT's fd 2 is put back through the static CRT so that no later test's stderr goes through a stale
    // value. Its _dup2 CloseHandle()s that value first: when the decoy took it, this is what closes the decoy, hence
    // no separate CloseHandle in that case; when it did not, the value is unused and the close is a harmless failure.
    EXPECT_EQ(::_dup2(saved_static, 2), 0) << "restoring the static CRT's fd 2, errno " << errno;
    ::_close(saved_static);
    if (!decoy_took_stale_handle)
        ::CloseHandle(decoy_handle);
#else
    ASSERT_EQ(::dup2(saved, 2), 2);
    ::close(saved);
    ::close(decoy_fd);
#endif

    const std::string captured = read_file(capture_path);
    const std::string decoy = read_file(decoy_path);
    std::filesystem::remove(capture_path);
    std::filesystem::remove(decoy_path);
    ASSERT_NE(captured.find("captured-line-42"), std::string::npos) << "got: " << captured;
    ASSERT_TRUE(decoy.empty()) << "written through a stale fd 2, got: " << decoy;
}
