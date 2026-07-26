#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct s_log_line {
    uint64_t seq;
    std::string text;
};

// Captures the x64dbg GUI log window for MCP clients. The plugin SDK has no log
// callback, so this works by dumping the log document to a temp file via
// GuiFlushLog() + GuiLogSave() and diffing against the previous dump. Newly
// completed lines are kept in a bounded ring buffer with monotonic sequence
// numbers so clients can page incrementally with a cursor.
//
// Threading: all public methods are safe to call from HTTP worker threads and
// serialize on an internal timed mutex. They must NOT be called from plugin
// event callbacks (x64dbg debug thread) — a capture waits on the GUI thread,
// and the GUI thread may block on the debug thread, which would deadlock.
//
// No method here may log via _plugin_logprintf/_plugin_logputs on the capture
// path: that would append to the very document being captured. Errors are
// reported exclusively by throwing std::runtime_error.
class c_log_capture {
public:
    // Incremental read from `cursor` (a previous next_cursor; 0 = oldest retained).
    // Captures fresh GUI content only when the buffered range cannot satisfy the
    // request. Returns lines plus paging metadata.
    [[nodiscard]] nlohmann::json read(uint64_t cursor, size_t max_lines);

    // Always captures, then returns the last `max_lines` complete lines.
    [[nodiscard]] nlohmann::json tail(size_t max_lines);

    // Clears the GUI log window AND the retained buffer. Runs a barrier capture so
    // the next read cannot observe pre-clear content. Sequence numbers are not reset.
    [[nodiscard]] nlohmann::json clear_all();

    // Writes `text` into the GUI log via _plugin_logputs and captures it back,
    // returning the cursor range that brackets the marker.
    [[nodiscard]] nlohmann::json write_marker(const std::string& text);

    // Buffer/GUI state introspection. Does not capture.
    [[nodiscard]] nlohmann::json status();

    // Called from plugstop: blocks further captures and sweeps temp files.
    // Deliberately not done in the destructor (which runs under the loader lock).
    void shutdown();

private:
    mutable std::timed_mutex m_mutex;
    std::atomic<bool> m_shutting_down{false};

    std::deque<s_log_line> m_lines;
    size_t   m_buffer_bytes = 0;
    uint64_t m_next_seq = 0;
    std::string m_partial;       // trailing text of the last dump with no '\n' yet

    size_t   m_last_len = 0;     // byte length of the previous full dump
    uint64_t m_last_hash = 0;    // FNV-1a of the previous full dump (seeded lazily)
    uint64_t m_capture_seq = 0;  // unique temp-file counter (seeded lazily)
    uint64_t m_epoch = 0;        // identifies this cursor space across plugin reloads

    uint32_t m_gui_clear_count = 0;
    bool     m_cleared_flag = false;  // sticky until consumed by the next read/tail

    std::vector<std::wstring> m_orphan_files;  // dumps we failed to delete; retried later

    // All private helpers assume m_mutex is held.
    void ensure_init_locked();
    void capture_locked();
    [[nodiscard]] std::string dump_gui_log_locked();
    void append_text_locked(const std::string& raw);
    void evict_locked();
    [[nodiscard]] std::wstring make_temp_path_locked();
    void sweep_orphans_locked();
    void sweep_all_temp_locked();
    [[nodiscard]] nlohmann::json make_page_locked(uint64_t cursor, size_t max_lines);
};

c_log_capture& get_log_capture();
