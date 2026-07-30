#include "bridge/c_bridge_executor.h"

#include <thread>
#include <chrono>
#include <cstring>

#include "bridgemain.h"
#include "_plugins.h"
#include "_dbgfunctions.h"
#include "util/format_utils.h"

namespace {

constexpr auto BREAKPOINT_API_UNAVAILABLE =
    "This x64dbg build does not expose the required breakpoint reference API";

[[nodiscard]] bool has_breakpoint_lookup_api(const DBGFUNCTIONS* functions) {
    return functions
        && functions->BpRefVa
        && functions->BpRefExists;
}

[[nodiscard]] bool has_breakpoint_list_api(const DBGFUNCTIONS* functions) {
    return has_breakpoint_lookup_api(functions)
        && functions->BpRefList
        && functions->BpGetFieldNumber
        && functions->BpGetFieldText;
}

[[nodiscard]] bool has_breakpoint_number_api(const DBGFUNCTIONS* functions) {
    return has_breakpoint_lookup_api(functions)
        && functions->BpGetFieldNumber
        && functions->BpSetFieldNumber;
}

[[nodiscard]] bool has_breakpoint_text_api(const DBGFUNCTIONS* functions) {
    return has_breakpoint_lookup_api(functions)
        && functions->BpGetFieldText
        && functions->BpSetFieldText;
}

} // namespace

static c_bridge_executor g_bridge;

c_bridge_executor& get_bridge() {
    return g_bridge;
}

bool c_bridge_executor::is_debugging() const {
    return DbgIsDebugging();
}

bool c_bridge_executor::is_running() const {
    return DbgIsRunning();
}

std::string c_bridge_executor::get_state_string() const {
    if (!DbgIsDebugging()) return "stopped";
    if (DbgIsRunning())    return "running";
    return "paused";
}

bool c_bridge_executor::exec_command(const std::string& cmd) {
    return DbgCmdExecDirect(cmd.c_str());
}

bool c_bridge_executor::exec_command_async(const std::string& cmd) {
    return DbgCmdExec(cmd.c_str());
}

bool c_bridge_executor::exec_command_and_wait(const std::string& cmd, int timeout_ms) {
    {
        std::lock_guard lock(m_mutex);
        if (!DbgCmdExecDirect(cmd.c_str())) {
            return false;
        }
    }
    // Wait outside lock: wait_for_pause only polls DbgIsRunning() which is thread-safe.
    // This avoids blocking all other bridge operations for the entire wait duration.
    return wait_for_pause(timeout_ms);
}

duint c_bridge_executor::exec_command_and_eval(const std::string& cmd, const std::string& expression) {
    std::lock_guard lock(m_mutex);
    if (!DbgCmdExecDirect(cmd.c_str())) {
        return 0;
    }
    return DbgValFromString(expression.c_str());
}

bool c_bridge_executor::wait_for_pause(int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeout_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    while (DbgIsRunning()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= timeout) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

duint c_bridge_executor::eval_expression(const std::string& expression) {
    return DbgValFromString(expression.c_str());
}

bool c_bridge_executor::is_valid_expression(const std::string& expression) {
    return DbgIsValidExpression(expression.c_str());
}

std::expected<std::vector<uint8_t>, std::string> c_bridge_executor::read_memory(duint address, size_t size) {
    if (size == 0 || size > 10 * 1024 * 1024) {
        return std::unexpected("Invalid read size (must be 1 to 10MB)");
    }
    std::vector<uint8_t> buffer(size);
    if (!DbgMemRead(address, buffer.data(), static_cast<duint>(size))) {
        return std::unexpected("Failed to read memory at " + format_utils::format_address(address));
    }
    return buffer;
}

std::expected<void, std::string> c_bridge_executor::write_memory(duint address, const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return std::unexpected("No data to write");
    }
    if (!DbgMemWrite(address, data.data(), static_cast<duint>(data.size()))) {
        return std::unexpected("Failed to write memory at " + format_utils::format_address(address));
    }
    return {};
}

bool c_bridge_executor::is_valid_read_ptr(duint address) {
    return DbgMemIsValidReadPtr(address);
}

std::expected<duint, std::string> c_bridge_executor::get_memory_region_base(duint address) {
    duint region_size = 0;
    const auto base = DbgMemFindBaseAddr(address, &region_size);
    if (base == 0 || region_size == 0) {
        return std::unexpected(
            "No memory region contains " + format_utils::format_address(address));
    }
    return base;
}

std::expected<REGDUMP, std::string> c_bridge_executor::get_register_dump() {
    // Use a buffer large enough for REGDUMP_AVX512 (the API's declared parameter type)
    // even though we only request sizeof(REGDUMP) worth of data. This prevents a stack
    // overflow if the API implementation doesn't strictly respect the size parameter.
    alignas(REGDUMP_AVX512) char buffer[sizeof(REGDUMP_AVX512)]{};
    if (!DbgGetRegDumpEx(reinterpret_cast<REGDUMP_AVX512*>(buffer), sizeof(REGDUMP))) {
        return std::unexpected("Failed to get register dump");
    }
    REGDUMP dump;
    std::memcpy(&dump, buffer, sizeof(REGDUMP));
    return dump;
}

std::expected<nlohmann::json, std::string> c_bridge_executor::get_memory_map() {
    MEMMAP memmap{};
    if (!DbgMemMap(&memmap)) {
        return std::unexpected("Failed to get memory map");
    }
    auto result = nlohmann::json::array();
    for (int i = 0; i < memmap.count; ++i) {
        const auto& page = memmap.page[i];
        result.push_back({
            {"base",             format_utils::format_address(reinterpret_cast<duint>(page.mbi.BaseAddress))},
            {"allocation_base",  format_utils::format_address(reinterpret_cast<duint>(page.mbi.AllocationBase))},
            {"size",             static_cast<duint>(page.mbi.RegionSize)},
            {"size_hex",         format_utils::format_hex(static_cast<duint>(page.mbi.RegionSize))},
            {"state",            format_utils::format_mem_state(page.mbi.State)},
            {"protect",          format_utils::format_protection(page.mbi.Protect)},
            {"type",             format_utils::format_mem_type(page.mbi.Type)},
            {"info",             page.info}
        });
    }
    if (memmap.page) BridgeFree(memmap.page);
    return result;
}

std::expected<nlohmann::json, std::string> c_bridge_executor::get_breakpoint_list(BPXTYPE type) {
    std::lock_guard lock(m_mutex);
    const auto* functions = DbgFunctions();
    if (!has_breakpoint_list_api(functions))
        return std::unexpected(std::string(BREAKPOINT_API_UNAVAILABLE));

    duint count = 0;
    auto* refs = functions->BpRefList(&count);
    if (count != 0 && refs == nullptr)
        return std::unexpected("Failed to enumerate breakpoint references");

    struct s_scoped_refs {
        BP_REF* value;
        ~s_scoped_refs() {
            if (value) BridgeFree(value);
        }
    } scoped_refs{refs};

    auto result = nlohmann::json::array();
    for (duint i = 0; i < count; ++i) {
        auto& ref = refs[i];
        if (ref.type != type) continue;

        duint address = 0;
        duint hit_count = 0;
        bool enabled = false;
        bool active = false;
        bool singleshoot = false;
        bool fast_resume = false;
        bool silent = false;
        std::string name;
        std::string module;
        std::string break_condition;
        std::string log_text;
        std::string log_condition;
        std::string log_file;
        std::string command_text;
        std::string command_condition;

        const auto read_fields = [&] {
            return ref.GetField(bpf_address, address)
                && ref.GetField(bpf_enabled, enabled)
                && ref.GetField(bpf_active, active)
                && ref.GetField(bpf_singleshoot, singleshoot)
                && ref.GetField(bpf_name, name)
                && ref.GetField(bpf_module, module)
                && ref.GetField(bpf_hitcount, hit_count)
                && ref.GetField(bpf_fastresume, fast_resume)
                && ref.GetField(bpf_silent, silent)
                && ref.GetField(bpf_breakcondition, break_condition)
                && ref.GetField(bpf_logtext, log_text)
                && ref.GetField(bpf_logcondition, log_condition)
                && ref.GetField(bpf_logfile, log_file)
                && ref.GetField(bpf_commandtext, command_text)
                && ref.GetField(bpf_commandcondition, command_condition);
        };

        if (!read_fields()) {
            // BpRefList is a snapshot, while each field read takes a separate
            // x64dbg lock. A concurrently removed singleshoot breakpoint is a
            // stale row, not a failure of the remaining enumeration.
            if (!functions->BpRefExists(&ref))
                continue;
            if (!read_fields()) {
                if (!functions->BpRefExists(&ref))
                    continue;
                return std::unexpected("Failed to read breakpoint fields");
            }
        }

        result.push_back({
            {"address",           format_utils::format_address(address)},
            {"enabled",           enabled},
            {"active",            active},
            {"singleshoot",       singleshoot},
            {"name",              name},
            {"module",            module},
            {"hit_count",         hit_count},
            {"fast_resume",       fast_resume},
            {"silent",            silent},
            {"break_condition",   break_condition},
            {"log_text",          log_text},
            {"log_condition",     log_condition},
            {"log_file",          log_file},
            {"command_text",      command_text},
            {"command_condition", command_condition},
            {"type",              static_cast<int>(ref.type)}
        });
    }
    return result;
}

std::expected<void, std::string> c_bridge_executor::require_breakpoint_configuration_api() {
    std::lock_guard lock(m_mutex);
    const auto* functions = DbgFunctions();
    if (!has_breakpoint_number_api(functions)
        || !has_breakpoint_text_api(functions)) {
        return std::unexpected(std::string(BREAKPOINT_API_UNAVAILABLE));
    }
    return {};
}

std::expected<bool, std::string> c_bridge_executor::breakpoint_exists(
    BPXTYPE type, duint address
) {
    std::lock_guard lock(m_mutex);
    const auto* functions = DbgFunctions();
    if (!has_breakpoint_lookup_api(functions))
        return std::unexpected(std::string(BREAKPOINT_API_UNAVAILABLE));

    BP_REF ref{};
    return functions->BpRefVa(&ref, type, address)
        && functions->BpRefExists(&ref);
}

std::expected<void, std::string> c_bridge_executor::set_breakpoint_text_field(
    BPXTYPE type, duint address, BP_FIELD field, const std::string& value
) {
    if (value.find('\0') != std::string::npos)
        return std::unexpected("Breakpoint text fields must not contain an embedded NUL");

    std::lock_guard lock(m_mutex);
    const auto* functions = DbgFunctions();
    if (!has_breakpoint_text_api(functions))
        return std::unexpected(std::string(BREAKPOINT_API_UNAVAILABLE));

    BP_REF ref{};
    if (!functions->BpRefVa(&ref, type, address)
        || !functions->BpRefExists(&ref)) {
        return std::unexpected("Breakpoint does not exist at " + format_utils::format_address(address));
    }
    if (!ref.SetField(field, value))
        return std::unexpected("Failed to set breakpoint text field");

    std::string actual;
    if (!ref.GetField(field, actual) || actual != value)
        return std::unexpected("Breakpoint text field verification failed");
    return {};
}

std::expected<void, std::string> c_bridge_executor::set_breakpoint_log_text(
    BPXTYPE type, duint address, const std::string& value
) {
    if (value.find('\0') != std::string::npos)
        return std::unexpected("Breakpoint log text must not contain an embedded NUL");

    std::lock_guard lock(m_mutex);
    const auto* functions = DbgFunctions();
    if (!has_breakpoint_text_api(functions)
        || !has_breakpoint_number_api(functions)) {
        return std::unexpected(std::string(BREAKPOINT_API_UNAVAILABLE));
    }

    BP_REF ref{};
    if (!functions->BpRefVa(&ref, type, address)
        || !functions->BpRefExists(&ref)) {
        return std::unexpected(
            "Breakpoint does not exist at " + format_utils::format_address(address));
    }

    // Match x64dbg's BpSetLogText semantics without routing arbitrary text
    // through the command parser: a non-empty log expression is silent by
    // default, while clearing it restores the default breakpoint message.
    if (!ref.SetField(bpf_logtext, value))
        return std::unexpected("Failed to set breakpoint log text");
    if (!ref.SetField(bpf_silent, value.empty() ? 0 : 1))
        return std::unexpected("Failed to update breakpoint silent state");

    std::string actual_text;
    bool actual_silent = false;
    if (!ref.GetField(bpf_logtext, actual_text) || actual_text != value)
        return std::unexpected("Breakpoint log text verification failed");
    if (!ref.GetField(bpf_silent, actual_silent)
        || actual_silent != !value.empty()) {
        return std::unexpected("Breakpoint silent state verification failed");
    }
    return {};
}

std::expected<void, std::string> c_bridge_executor::set_breakpoint_number_field(
    BPXTYPE type, duint address, BP_FIELD field, duint value
) {
    std::lock_guard lock(m_mutex);
    const auto* functions = DbgFunctions();
    if (!has_breakpoint_number_api(functions))
        return std::unexpected(std::string(BREAKPOINT_API_UNAVAILABLE));

    BP_REF ref{};
    if (!functions->BpRefVa(&ref, type, address)
        || !functions->BpRefExists(&ref)) {
        return std::unexpected("Breakpoint does not exist at " + format_utils::format_address(address));
    }
    if (!ref.SetField(field, value))
        return std::unexpected("Failed to set breakpoint number field");

    duint actual = 0;
    if (!ref.GetField(field, actual) || actual != value)
        return std::unexpected("Breakpoint number field verification failed");
    return {};
}

std::expected<void, std::string> c_bridge_executor::set_breakpoint_singleshoot(
    BPXTYPE type, duint address, bool value
) {
    std::lock_guard lock(m_mutex);
    const auto* functions = DbgFunctions();
    if (!has_breakpoint_lookup_api(functions)
        || !functions->BpGetFieldNumber) {
        return std::unexpected(std::string(BREAKPOINT_API_UNAVAILABLE));
    }

    const auto read_state = [&](bool& enabled, bool& singleshoot) {
        BP_REF state_ref{};
        return functions->BpRefVa(&state_ref, type, address)
            && functions->BpRefExists(&state_ref)
            && state_ref.GetField(bpf_enabled, enabled)
            && state_ref.GetField(bpf_singleshoot, singleshoot);
    };

    bool was_enabled = false;
    bool previous_value = false;
    if (!read_state(was_enabled, previous_value)) {
        return std::unexpected(
            "Breakpoint does not exist at " + format_utils::format_address(address));
    }
    if (previous_value == value)
        return {};

    const char* setter = nullptr;
    const char* disable = nullptr;
    const char* enable = nullptr;
    switch (type) {
        case bp_normal:
            setter = "SetBreakpointSingleshoot ";
            disable = "bpd ";
            enable = "bpe ";
            break;
        case bp_hardware:
            // Hardware single-shot behavior is implemented by x64dbg's
            // breakpoint metadata handler; TitanEngine has no separate
            // single-shot state to synchronize.
            setter = "SetHardwareBreakpointSingleshoot ";
            break;
        case bp_memory:
            setter = "SetMemoryBreakpointSingleshoot ";
            disable = "bpmd ";
            enable = "bpme ";
            break;
        default:
            return std::unexpected("Unsupported breakpoint type for singleshoot");
    }

    const auto address_text = format_utils::format_address(address);
    const auto make_setter_command = [&](bool requested) {
        return std::string(setter) + address_text
            + ", " + (requested ? "1" : "0");
    };

    // BpSetSingleshoot updates metadata before asking TitanEngine to reinstall
    // an enabled software or memory breakpoint, and x64dbg only logs those
    // reinstall failures. Move the engine transition to the enable/disable
    // commands, which propagate their underlying operation result.
    const bool needs_engine_reinstall =
        was_enabled && (type == bp_normal || type == bp_memory);
    if (needs_engine_reinstall) {
        const auto disable_command = std::string(disable) + address_text;
        if (!DbgCmdExecDirect(disable_command.c_str()))
            return std::unexpected(
                "Failed to disable breakpoint before singleshoot update");

        bool enabled = true;
        bool singleshoot = previous_value;
        if (!read_state(enabled, singleshoot) || enabled)
            return std::unexpected(
                "Breakpoint disable verification failed before singleshoot update");
    }

    const auto setter_command = make_setter_command(value);
    if (!DbgCmdExecDirect(setter_command.c_str())) {
        if (needs_engine_reinstall) {
            const auto enable_command = std::string(enable) + address_text;
            DbgCmdExecDirect(enable_command.c_str());
        }
        return std::unexpected("Failed to dispatch breakpoint singleshoot update");
    }

    bool enabled_after_set = false;
    bool actual = false;
    if (!read_state(enabled_after_set, actual)
        || actual != value
        || (needs_engine_reinstall && enabled_after_set)) {
        return std::unexpected("Breakpoint singleshoot verification failed");
    }

    if (needs_engine_reinstall) {
        const auto enable_command = std::string(enable) + address_text;
        if (!DbgCmdExecDirect(enable_command.c_str())) {
            const auto restore_command = make_setter_command(previous_value);
            const bool restored =
                DbgCmdExecDirect(restore_command.c_str())
                && DbgCmdExecDirect(enable_command.c_str());
            return std::unexpected(
                std::string("Failed to re-enable breakpoint after singleshoot update")
                + (restored
                    ? "; the original singleshoot setting was restored"
                    : "; rollback also failed and the breakpoint may remain disabled"));
        }

        bool enabled = false;
        if (!read_state(enabled, actual) || !enabled || actual != value)
            return std::unexpected(
                "Breakpoint re-enable verification failed after singleshoot update");
    }
    return {};
}

std::expected<nlohmann::json, std::string> c_bridge_executor::get_thread_list() {
    THREADLIST thread_list{};
    DbgGetThreadList(&thread_list);
    auto result = nlohmann::json::object();
    auto threads = nlohmann::json::array();
    for (int i = 0; i < thread_list.count; ++i) {
        const auto& t = thread_list.list[i];
        threads.push_back({
            {"number",        t.BasicInfo.ThreadNumber},
            {"id",            t.BasicInfo.ThreadId},
            {"handle",        format_utils::format_address(reinterpret_cast<duint>(t.BasicInfo.Handle))},
            {"start_address", format_utils::format_address(t.BasicInfo.ThreadStartAddress)},
            {"local_base",    format_utils::format_address(t.BasicInfo.ThreadLocalBase)},
            {"name",          t.BasicInfo.threadName},
            {"cip",           format_utils::format_address(t.ThreadCip)},
            {"suspend_count", t.SuspendCount},
            {"priority",      static_cast<int>(t.Priority)},
            {"last_error",    t.LastError}
        });
    }
    result["threads"] = threads;
    result["count"] = thread_list.count;
    result["current_thread"] = thread_list.CurrentThread;
    if (thread_list.list) BridgeFree(thread_list.list);
    return result;
}

std::string c_bridge_executor::get_label_at(duint address) {
    char label[MAX_LABEL_SIZE] = {};
    if (DbgGetLabelAt(address, SEG_DEFAULT, label)) return label;
    return "";
}

bool c_bridge_executor::set_label_at(duint address, const std::string& text) {
    return DbgSetLabelAt(address, text.c_str());
}

std::string c_bridge_executor::get_comment_at(duint address) {
    char comment[MAX_COMMENT_SIZE] = {};
    if (DbgGetCommentAt(address, comment)) return comment;
    return "";
}

bool c_bridge_executor::set_comment_at(duint address, const std::string& text) {
    return DbgSetCommentAt(address, text.c_str());
}

bool c_bridge_executor::set_bookmark_at(duint address, bool set) {
    return DbgSetBookmarkAt(address, set);
}

std::expected<nlohmann::json, std::string> c_bridge_executor::disassemble_at(duint address, int count) {
    auto instructions = nlohmann::json::array();
    auto current_addr = address;
    for (int i = 0; i < count; ++i) {
        DISASM_INSTR instr{};
        DbgDisasmAt(current_addr, &instr);
        if (instr.instr_size == 0) break;
        BASIC_INSTRUCTION_INFO basic{};
        DbgDisasmFastAt(current_addr, &basic);
        char label[MAX_LABEL_SIZE] = {};
        DbgGetLabelAt(current_addr, SEG_DEFAULT, label);
        char comment[MAX_COMMENT_SIZE] = {};
        DbgGetCommentAt(current_addr, comment);
        instructions.push_back({
            {"address",     format_utils::format_address(current_addr)},
            {"instruction", instr.instruction},
            {"size",        instr.instr_size},
            {"type",        static_cast<int>(instr.type)},
            {"is_branch",   basic.branch},
            {"is_call",     basic.call},
            {"label",       label},
            {"comment",     comment}
        });
        current_addr += instr.instr_size;
    }
    return instructions;
}

std::expected<nlohmann::json, std::string> c_bridge_executor::get_basic_info(duint address) {
    BASIC_INSTRUCTION_INFO info{};
    DbgDisasmFastAt(address, &info);
    if (info.size == 0) {
        return std::unexpected("Failed to get instruction info at " + format_utils::format_address(address));
    }
    return nlohmann::json{
        {"address",     format_utils::format_address(address)},
        {"size",        info.size},
        {"is_branch",   info.branch},
        {"is_call",     info.call},
        {"instruction", info.instruction}
    };
}

std::expected<nlohmann::json, std::string> c_bridge_executor::get_function_bounds(duint address) {
    duint start = 0, end = 0;
    if (!DbgFunctionGet(address, &start, &end)) {
        return std::unexpected("No function found at " + format_utils::format_address(address));
    }
    // DbgFunctionGet returns an inclusive range [start, end], so size includes both endpoints
    return nlohmann::json{
        {"start", format_utils::format_address(start)},
        {"end",   format_utils::format_address(end)},
        {"size",  end - start + 1}
    };
}

duint c_bridge_executor::get_module_base(const std::string& name) {
    return DbgModBaseFromName(name.c_str());
}

std::string c_bridge_executor::get_module_at(duint address) {
    char mod_name[MAX_MODULE_SIZE] = {};
    if (DbgGetModuleAt(address, mod_name)) return mod_name;
    return "";
}

bool c_bridge_executor::require_paused() const {
    return is_debugging() && !is_running();
}

bool c_bridge_executor::require_debugging() const {
    return is_debugging();
}
