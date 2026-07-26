#include "bridge/c_log_capture.h"
#include "util/format_utils.h"

#include "bridgemain.h"
#include "_plugins.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace {

// Ring buffer bounds
constexpr size_t MAX_LINES = 10000;
constexpr size_t MAX_BUFFER_BYTES = 4 * 1024 * 1024;
constexpr size_t LINE_OVERHEAD = 48;  // approximate std::string + deque bookkeeping per line

// Capture timing
constexpr DWORD POLL_INTERVAL_MS = 10;
constexpr ULONGLONG CAPTURE_TIMEOUT_MS = 2000;
constexpr auto LOCK_TIMEOUT = std::chrono::seconds(5);

// Response paging
constexpr size_t MAX_PAGE_CHARS = 64 * 1024;

constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

[[nodiscard]] uint64_t fnv1a(const char* data, size_t len) {
    uint64_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= FNV_PRIME;
    }
    return hash;
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    auto needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (needed <= 0) throw std::runtime_error("WideCharToMultiByte failed for temp path");
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

[[nodiscard]] std::wstring temp_directory() {
    // GetTempPathW output can exceed MAX_PATH — two-call sizing.
    auto needed = GetTempPathW(0, nullptr);
    if (needed == 0)
        throw std::runtime_error("GetTempPathW failed (error " + std::to_string(GetLastError()) + ")");
    std::wstring dir(needed, L'\0');
    auto written = GetTempPathW(needed, dir.data());
    if (written == 0 || written > needed)
        throw std::runtime_error("GetTempPathW failed (error " + std::to_string(GetLastError()) + ")");
    dir.resize(written);  // includes the trailing backslash
    return dir;
}

[[nodiscard]] std::wstring temp_file_prefix() {
    return L"x64dbg_mcp_log_" + std::to_wstring(GetCurrentProcessId()) + L"_";
}

}  // namespace

static c_log_capture g_log_capture;

c_log_capture& get_log_capture() {
    return g_log_capture;
}

void c_log_capture::ensure_init_locked() {
    if (m_shutting_down.load()) throw std::runtime_error("MCP plugin is shutting down");
    if (m_epoch != 0) return;
    // Seed lazily on first use (not in the constructor, which runs at DLL load).
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    m_epoch = (static_cast<uint64_t>(GetCurrentProcessId()) << 48)
            ^ static_cast<uint64_t>(qpc.QuadPart)
            ^ (static_cast<uint64_t>(GetTickCount64()) << 16);
    if (m_epoch == 0) m_epoch = 1;
    // Seed the temp-file counter so a plugin reload in the same process cannot
    // reuse a filename that a previous instance's abandoned save might still create.
    m_capture_seq = GetTickCount64();
    m_last_hash = FNV_OFFSET_BASIS;  // hash of the empty prefix
}

std::wstring c_log_capture::make_temp_path_locked() {
    return temp_directory() + temp_file_prefix() + std::to_wstring(m_capture_seq++) + L".tmp";
}

void c_log_capture::sweep_orphans_locked() {
    if (m_orphan_files.empty()) return;
    std::erase_if(m_orphan_files, [](const std::wstring& path) {
        return DeleteFileW(path.c_str()) != 0
            || GetLastError() == ERROR_FILE_NOT_FOUND
            || GetLastError() == ERROR_PATH_NOT_FOUND;
    });
}

void c_log_capture::sweep_all_temp_locked() {
    std::wstring dir;
    try {
        dir = temp_directory();
    } catch (const std::exception&) {
        return;  // shutdown path — nothing sensible to do
    }
    WIN32_FIND_DATAW find{};
    auto pattern = dir + temp_file_prefix() + L"*.tmp";
    HANDLE h = FindFirstFileW(pattern.c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileW((dir + find.cFileName).c_str());
    } while (FindNextFileW(h, &find));
    FindClose(h);
}

std::string c_log_capture::dump_gui_log_locked() {
    const auto path = make_temp_path_locked();

    // Both calls are queued to the GUI thread and executed in emission order, so the
    // flush slot (logBuffer -> document) runs before the save slot dumps the document.
    GuiFlushLog();
    GuiLogSave(wide_to_utf8(path).c_str());

    // The save slot opens the file, writes document()->toPlainText().toUtf8(), and
    // closes. Completion probe: open denying write sharing (FILE_SHARE_READ only).
    // While the GUI's QFile still holds its write handle this fails with
    // ERROR_SHARING_VIOLATION; once it succeeds the file is complete, and because the
    // filename is unique per capture nothing will ever write to it again.
    const ULONGLONG deadline = GetTickCount64() + CAPTURE_TIMEOUT_MS;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (;;) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const auto err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND
            && err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
            m_orphan_files.push_back(path);
            throw std::runtime_error("Failed to open log dump file (error " + std::to_string(err) + ")");
        }
        if (GetTickCount64() >= deadline) {
            // The queued save may still complete later; remember the path so the
            // stray file gets deleted on a later capture or at shutdown.
            m_orphan_files.push_back(path);
            throw std::runtime_error(
                "x64dbg GUI did not produce a log dump within 2000ms "
                "(GUI thread busy or a modal dialog is open)");
        }
        Sleep(POLL_INTERVAL_MS);
    }

    std::string content;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
        content.resize(static_cast<size_t>(size.QuadPart));
        size_t total = 0;
        while (total < content.size()) {
            const auto chunk = static_cast<DWORD>((std::min)(content.size() - total, size_t{1} << 20));
            DWORD got = 0;
            if (!ReadFile(file, content.data() + total, chunk, &got, nullptr) || got == 0) break;
            total += got;
        }
        content.resize(total);
    }
    CloseHandle(file);
    if (!DeleteFileW(path.c_str())) m_orphan_files.push_back(path);
    return content;
}

void c_log_capture::capture_locked() {
    sweep_orphans_locked();

    auto dump = dump_gui_log_locked();
    auto prefix_matches = [this](const std::string& d) {
        return d.size() >= m_last_len && fnv1a(d.data(), m_last_len) == m_last_hash;
    };

    if (prefix_matches(dump)) {
        append_text_locked(dump.substr(m_last_len));
    } else {
        // Prefix mismatch: the GUI log was cleared (user action or the 1 MiB document
        // purge) — or this dump was torn. One fresh dump disambiguates: a torn read
        // does not reproduce, a cleared document does.
        dump = dump_gui_log_locked();
        if (prefix_matches(dump)) {
            append_text_locked(dump.substr(m_last_len));
        } else {
            m_cleared_flag = true;
            ++m_gui_clear_count;
            m_partial.clear();
            append_text_locked(dump);  // entire post-clear document is fresh content
        }
    }

    m_last_len = dump.size();
    m_last_hash = fnv1a(dump.data(), dump.size());
}

void c_log_capture::append_text_locked(const std::string& raw) {
    if (raw.empty()) return;
    std::string text = std::move(m_partial);
    text += raw;
    m_partial.clear();

    size_t start = 0;
    for (;;) {
        const auto nl = text.find('\n', start);
        if (nl == std::string::npos) {
            m_partial.assign(text, start, std::string::npos);
            break;
        }
        // QIODevice::Text saves CRLF; a line already ending in '\r' becomes "\r\r\n".
        // Strip every trailing '\r' to recover what the log window displays.
        auto end = nl;
        while (end > start && text[end - 1] == '\r') --end;
        m_lines.push_back({m_next_seq++, text.substr(start, end - start)});
        m_buffer_bytes += m_lines.back().text.size() + LINE_OVERHEAD;
        start = nl + 1;
    }
    evict_locked();
}

void c_log_capture::evict_locked() {
    while (!m_lines.empty() && (m_lines.size() > MAX_LINES || m_buffer_bytes > MAX_BUFFER_BYTES)) {
        m_buffer_bytes -= m_lines.front().text.size() + LINE_OVERHEAD;
        m_lines.pop_front();
    }
}

nlohmann::json c_log_capture::make_page_locked(uint64_t cursor, size_t max_lines) {
    const uint64_t oldest = m_lines.empty() ? m_next_seq : m_lines.front().seq;

    uint64_t dropped_count = 0;
    if (cursor < oldest) {
        dropped_count = oldest - cursor;
        cursor = oldest;
    }
    if (cursor > m_next_seq) cursor = m_next_seq;  // stale/foreign cursor from before a reload

    auto lines = nlohmann::json::array();
    size_t chars = 0;
    uint64_t seq = cursor;
    while (seq < m_next_seq && lines.size() < max_lines) {
        const auto& line = m_lines[static_cast<size_t>(seq - oldest)];
        // Char cap guards the MCP response size; always emit at least one line so a
        // single oversized line cannot wedge paging (next_cursor == cursor forever).
        if (!lines.empty() && chars + line.text.size() > MAX_PAGE_CHARS) break;
        lines.push_back(line.text);
        chars += line.text.size();
        ++seq;
    }

    const bool cleared = m_cleared_flag;
    m_cleared_flag = false;

    return {
        {"lines", std::move(lines)},
        {"first_cursor", cursor},
        {"next_cursor", seq},
        {"dropped", dropped_count > 0},
        {"dropped_count", dropped_count},
        {"truncated", seq < m_next_seq},
        {"log_cleared", cleared},
        {"partial", format_utils::to_utf8_safe(m_partial)},
        {"log_enabled", GuiIsLogEnabled()},
        {"capture_epoch", m_epoch},
    };
}

nlohmann::json c_log_capture::read(uint64_t cursor, size_t max_lines) {
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (!lock.try_lock_for(LOCK_TIMEOUT))
        throw std::runtime_error("log capture busy (another client holds the capture lock)");
    ensure_init_locked();
    // Only hit the GUI when the buffered range cannot satisfy the request; paging
    // through already-captured history must not re-dump the document per page.
    if (cursor >= m_next_seq || m_next_seq - cursor < max_lines) capture_locked();
    return make_page_locked(cursor, max_lines);
}

nlohmann::json c_log_capture::tail(size_t max_lines) {
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (!lock.try_lock_for(LOCK_TIMEOUT))
        throw std::runtime_error("log capture busy (another client holds the capture lock)");
    ensure_init_locked();
    capture_locked();
    const uint64_t oldest = m_lines.empty() ? m_next_seq : m_lines.front().seq;
    uint64_t cursor = m_next_seq - (std::min)(static_cast<uint64_t>(max_lines), m_next_seq);
    if (cursor < oldest) cursor = oldest;
    return make_page_locked(cursor, max_lines);
}

nlohmann::json c_log_capture::clear_all() {
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (!lock.try_lock_for(LOCK_TIMEOUT))
        throw std::runtime_error("log capture busy (another client holds the capture lock)");
    ensure_init_locked();

    GuiLogClear();
    m_lines.clear();
    m_buffer_bytes = 0;
    m_partial.clear();

    // Barrier: the dump below is queued after the clear slot, so it provably reflects
    // the post-clear document. Anything that raced in right after the clear is kept.
    auto dump = dump_gui_log_locked();
    append_text_locked(dump);
    m_last_len = dump.size();
    m_last_hash = fnv1a(dump.data(), dump.size());
    m_cleared_flag = false;  // our own clear is not a surprise worth flagging

    return {
        {"cleared", true},
        {"next_cursor", m_next_seq},
        {"capture_epoch", m_epoch},
    };
}

nlohmann::json c_log_capture::write_marker(const std::string& text) {
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (!lock.try_lock_for(LOCK_TIMEOUT))
        throw std::runtime_error("log capture busy (another client holds the capture lock)");
    ensure_init_locked();

    const uint64_t cursor_before = m_next_seq;
    _plugin_logputs(text.c_str());  // queued: [add msg] -> [flush] -> [save] below
    capture_locked();

    return {
        {"written", true},
        {"cursor_before", cursor_before},
        {"next_cursor", m_next_seq},
        {"capture_epoch", m_epoch},
    };
}

nlohmann::json c_log_capture::status() {
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (!lock.try_lock_for(LOCK_TIMEOUT))
        throw std::runtime_error("log capture busy (another client holds the capture lock)");
    ensure_init_locked();
    const uint64_t oldest = m_lines.empty() ? m_next_seq : m_lines.front().seq;
    return {
        {"log_enabled", GuiIsLogEnabled()},
        {"retained_lines", m_lines.size()},
        {"oldest_cursor", oldest},
        {"next_cursor", m_next_seq},
        {"gui_clear_count", m_gui_clear_count},
        {"buffer_bytes", m_buffer_bytes},
        {"doc_bytes", m_last_len},  // GUI document size at last capture; purged wholesale at 1 MiB
        {"capture_epoch", m_epoch},
    };
}

void c_log_capture::shutdown() {
    m_shutting_down.store(true);
    // HTTP workers are already joined by c_http_server::stop() before plugstop calls
    // this, so the lock should be uncontended; the timeout is a paranoia backstop.
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(2))) return;
    sweep_orphans_locked();
    sweep_all_temp_locked();
}
