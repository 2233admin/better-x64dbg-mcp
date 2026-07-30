#include "handlers/breakpoint_handler.h"
#include "bridge/c_bridge_executor.h"
#include "util/format_utils.h"
#include "util/path_utils.h"
#include "bridgemain.h"
#include "_dbgfunctions.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <string_view>

namespace handlers::breakpoints {

static BPXTYPE bridge_bp_type(const std::string& bp_type) {
    if (bp_type == "software") return bp_normal;
    if (bp_type == "hardware") return bp_hardware;
    if (bp_type == "memory") return bp_memory;
    throw std::runtime_error("Invalid bp_type '" + bp_type
                             + "', expected: software, hardware, memory");
}

static duint evaluate_config_address(
    c_bridge_executor& bridge,
    const std::string& expression
) {
    if (!bridge.is_valid_expression(expression))
        throw std::runtime_error("Invalid breakpoint address expression '" + expression + "'");
    return bridge.eval_expression(expression);
}

static duint resolve_breakpoint_address(
    c_bridge_executor& bridge,
    duint address,
    BPXTYPE bp_type
) {
    if (bp_type != bp_memory) return address;

    // bpm refreshes x64dbg's memory map before normalizing the requested address.
    // Resolve from that refreshed map after the command has succeeded.
    const auto base = bridge.get_memory_region_base(address);
    if (!base) throw std::runtime_error(base.error());
    return *base;
}

static std::string make_memory_breakpoint_command(
    const std::string& address,
    const nlohmann::json& body
) {
    // bpm's second argument is "restore", the inverse of singleshot. Pass it
    // during creation so TitanEngine and x64dbg metadata start in agreement;
    // the semantic setter below still handles an already-existing breakpoint.
    const auto restore = body.value("singleshot", false) ? "0" : "1";
    return "bpm " + address + ", " + restore
        + ", " + body.value("mem_type", "a");
}

static void validate_log_file(const nlohmann::json& body) {
    if (!body.contains("log_file")) return;
    const auto& path = body["log_file"].get_ref<const std::string&>();
    if (path.empty()) return; // empty disables breakpoint file logging
    if (path_utils::contains_embedded_nul(path))
        throw std::runtime_error("log_file must not contain an embedded NUL");
    if (!path_utils::is_absolute_windows_path(path))
        throw std::runtime_error("log_file must be an absolute Windows path");
}

static void validate_string_field(
    const nlohmann::json& body,
    const char* name,
    bool required = false
) {
    if (!body.contains(name)) {
        if (required)
            throw std::runtime_error(std::string("missing ") + name);
        return;
    }
    if (!body[name].is_string())
        throw std::runtime_error(std::string(name) + " must be a string");

    const auto& value = body[name].get_ref<const std::string&>();
    if (value.find('\0') != std::string::npos)
        throw std::runtime_error(std::string(name) + " must not contain an embedded NUL");
}

static void validate_bool_field(
    const nlohmann::json& body,
    const char* name
) {
    if (body.contains(name) && !body[name].is_boolean())
        throw std::runtime_error(std::string(name) + " must be a boolean");
}

static void validate_enum_field(
    const nlohmann::json& body,
    const char* name,
    std::initializer_list<std::string_view> allowed
) {
    if (!body.contains(name)) return;
    validate_string_field(body, name);

    const auto& value = body[name].get_ref<const std::string&>();
    if ((std::find)(
            allowed.begin(), allowed.end(), std::string_view(value))
        == allowed.end()) {
        throw std::runtime_error(
            std::string("Invalid ") + name + " '" + value + "'");
    }
}

static void validate_bp_config(const nlohmann::json& body) {
    if (!body.is_object())
        throw std::runtime_error("breakpoint entry must be an object");

    validate_string_field(body, "address", true);
    validate_enum_field(
        body, "bp_type", {"software", "hardware", "memory"});
    validate_enum_field(body, "hw_type", {"r", "w", "x"});
    validate_enum_field(body, "hw_size", {"1", "2", "4", "8"});
    validate_enum_field(body, "mem_type", {"a", "r", "w", "x"});

    static constexpr std::array text_fields = {
        "break_condition",
        "command_condition",
        "command_text",
        "log_text",
        "log_condition",
        "log_file",
        "name",
    };
    for (const auto* field : text_fields)
        validate_string_field(body, field);

    validate_bool_field(body, "singleshot");
    validate_bool_field(body, "silent");
    validate_bool_field(body, "fast_resume");
    validate_log_file(body);
}

static void apply_bp_config(
    c_bridge_executor& bridge,
    duint address,
    BPXTYPE bp_type,
    const nlohmann::json& body
) {
    auto set_text = [&](const char* name, BP_FIELD field) {
        if (!body.contains(name)) return;
        const auto& value = body[name].get_ref<const std::string&>();
        auto result = field == bpf_logtext
            ? bridge.set_breakpoint_log_text(bp_type, address, value)
            : bridge.set_breakpoint_text_field(bp_type, address, field, value);
        if (!result)
            throw std::runtime_error(std::string("Failed to set ") + name + ": " + result.error());
    };
    auto set_bool = [&](const char* name, BP_FIELD field) {
        if (!body.contains(name)) return;
        auto result = bridge.set_breakpoint_number_field(
            bp_type, address, field, body[name].get<bool>() ? 1 : 0);
        if (!result)
            throw std::runtime_error(std::string("Failed to set ") + name + ": " + result.error());
    };

    set_text("break_condition", bpf_breakcondition);
    set_text("command_condition", bpf_commandcondition);
    set_text("command_text", bpf_commandtext);
    set_text("log_text", bpf_logtext);
    set_text("log_condition", bpf_logcondition);
    set_text("log_file", bpf_logfile);
    set_text("name", bpf_name);
    if (body.contains("singleshot")) {
        auto result = bridge.set_breakpoint_singleshoot(
            bp_type, address, body["singleshot"].get<bool>());
        if (!result)
            throw std::runtime_error(
                "Failed to set singleshot: " + result.error());
    }
    set_bool("silent", bpf_silent);
    set_bool("fast_resume", bpf_fastresume);
    GuiUpdateBreakpointsView();
}

static bool setup_breakpoint(
    c_bridge_executor& bridge,
    const std::string& command_address,
    BPXTYPE bp_type,
    const nlohmann::json& body
) {
    if (bp_type == bp_hardware) {
        return bridge.exec_command(
            "bphws " + command_address + ", "
            + body.value("hw_type", "x") + ", "
            + body.value("hw_size", "1"));
    }
    if (bp_type == bp_memory) {
        return bridge.exec_command(
            make_memory_breakpoint_command(command_address, body));
    }
    return bridge.exec_command(
        body.value("singleshot", false)
            ? "bp " + command_address + ", ss"
            : "bp " + command_address);
}

static std::string breakpoint_delete_command(
    BPXTYPE type,
    const std::string& address
) {
    switch (type) {
        case bp_normal:   return "bc " + address;
        case bp_hardware: return "bphwc " + address;
        case bp_memory:   return "bpmc " + address;
        default: throw std::runtime_error("Unsupported breakpoint type for deletion");
    }
}

static std::string breakpoint_enabled_command(
    BPXTYPE type,
    const std::string& address,
    bool enabled
) {
    switch (type) {
        case bp_normal:   return (enabled ? "bpe " : "bpd ") + address;
        case bp_hardware: return (enabled ? "bphwe " : "bphwd ") + address;
        case bp_memory:   return (enabled ? "bpme " : "bpmd ") + address;
        default: throw std::runtime_error("Unsupported breakpoint type for state update");
    }
}

static std::optional<nlohmann::json> find_breakpoint_snapshot(
    const nlohmann::json& breakpoints,
    const std::string& address
) {
    for (const auto& breakpoint : breakpoints) {
        if (breakpoint.value("address", "") == address)
            return breakpoint;
    }
    return std::nullopt;
}

static nlohmann::json config_from_snapshot(
    const nlohmann::json& snapshot
) {
    return {
        {"break_condition", snapshot.at("break_condition")},
        {"command_condition", snapshot.at("command_condition")},
        {"command_text", snapshot.at("command_text")},
        {"log_text", snapshot.at("log_text")},
        {"log_condition", snapshot.at("log_condition")},
        {"log_file", snapshot.at("log_file")},
        {"name", snapshot.at("name")},
        {"singleshot", snapshot.at("singleshoot")},
        {"silent", snapshot.at("silent")},
        {"fast_resume", snapshot.at("fast_resume")},
    };
}

// Returns an empty string on success, otherwise a concise rollback error.
static std::string rollback_breakpoint(
    c_bridge_executor& bridge,
    BPXTYPE type,
    duint address,
    const std::optional<nlohmann::json>& snapshot
) {
    const auto address_text = format_utils::format_address(address);
    try {
        if (!snapshot) {
            const auto existed = bridge.breakpoint_exists(type, address);
            if (!existed)
                return "failed to query new breakpoint before deletion: " + existed.error();
            if (!*existed)
                return {};

            if (!bridge.exec_command(
                    breakpoint_delete_command(type, address_text))) {
                return "failed to dispatch breakpoint deletion";
            }

            const auto exists = bridge.breakpoint_exists(type, address);
            if (!exists)
                return "failed to verify breakpoint deletion: " + exists.error();
            if (*exists)
                return "new breakpoint still exists after deletion";
            GuiUpdateBreakpointsView();
            return {};
        }

        apply_bp_config(
            bridge, address, type, config_from_snapshot(*snapshot));

        const auto was_enabled = snapshot->at("enabled").get<bool>();
        auto current = bridge.get_breakpoint_list(type);
        if (!current)
            return "failed to verify restored breakpoint: " + current.error();
        auto restored = find_breakpoint_snapshot(*current, address_text);
        if (!restored)
            return "breakpoint disappeared during rollback";
        if (restored->at("enabled").get<bool>() != was_enabled) {
            if (!bridge.exec_command(
                    breakpoint_enabled_command(type, address_text, was_enabled))) {
                return "failed to restore breakpoint enabled state";
            }

            current = bridge.get_breakpoint_list(type);
            if (!current)
                return "failed to verify restored breakpoint: " + current.error();
            restored = find_breakpoint_snapshot(*current, address_text);
            if (!restored)
                return "breakpoint disappeared during rollback";
        }
        if (restored->at("enabled").get<bool>() != was_enabled)
            return "breakpoint enabled state was not restored";
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

static nlohmann::json configure_one(
    c_bridge_executor& bridge,
    const nlohmann::json& body
) {
    static std::mutex config_mutex;
    std::lock_guard config_lock(config_mutex);

    // Complete all deterministic validation before a command can create or
    // re-enable a breakpoint.
    validate_bp_config(body);
    const auto& address = body["address"].get_ref<const std::string&>();
    const auto bp_type_name = body.value("bp_type", "software");
    const auto bp_type = bridge_bp_type(bp_type_name);

    const auto api = bridge.require_breakpoint_configuration_api();
    if (!api)
        throw std::runtime_error(api.error());

    const auto before = bridge.get_breakpoint_list(bp_type);
    if (!before)
        throw std::runtime_error(before.error());

    const auto requested_address =
        evaluate_config_address(bridge, address);
    const auto command_address =
        format_utils::format_address(requested_address);
    const auto setup_ok =
        setup_breakpoint(bridge, command_address, bp_type, body);

    const auto resolved_address =
        resolve_breakpoint_address(bridge, requested_address, bp_type);
    const auto resolved_text =
        format_utils::format_address(resolved_address);
    const auto snapshot =
        find_breakpoint_snapshot(*before, resolved_text);
    const auto exists =
        bridge.breakpoint_exists(bp_type, resolved_address);
    if (!exists)
        throw std::runtime_error(exists.error());

    if (!setup_ok || !*exists) {
        std::string message = !setup_ok
            ? "Failed to set " + bp_type_name + " breakpoint at " + address
            : "x64dbg did not create the " + bp_type_name
                + " breakpoint at " + address;
        if (*exists) {
            const auto rollback =
                rollback_breakpoint(bridge, bp_type, resolved_address, snapshot);
            message += rollback.empty()
                ? "; changes were rolled back"
                : "; rollback failed: " + rollback;
        }
        throw std::runtime_error(message);
    }

    try {
        apply_bp_config(
            bridge, resolved_address, bp_type, body);
    } catch (const std::exception& e) {
        const auto rollback =
            rollback_breakpoint(bridge, bp_type, resolved_address, snapshot);
        throw std::runtime_error(
            std::string(e.what())
            + (rollback.empty()
                ? "; changes were rolled back"
                : "; rollback failed: " + rollback));
    }

    return {
        {"address", resolved_text},
        {"bp_type", bp_type_name},
        {"configured", true}
    };
}

nlohmann::json list() {
    auto& bridge = get_bridge();
    nlohmann::json all_bps = nlohmann::json::array();
    auto add_bps = [&](BPXTYPE type, const char* type_name) {
        auto bps = bridge.get_breakpoint_list(type);
        if (!bps)
            throw std::runtime_error(bps.error());
        for (auto& bp : bps.value()) {
            if (bp["name"].get<std::string>().empty()) {
                auto addr = bridge.eval_expression(bp["address"].get<std::string>());
                bp["label"] = bridge.get_label_at(addr);
            } else {
                bp["label"] = bp["name"].get<std::string>();
            }
            bp["type_name"] = type_name;
            all_bps.push_back(bp);
        }
    };
    add_bps(bp_normal, "software");
    add_bps(bp_hardware, "hardware");
    add_bps(bp_memory, "memory");
    return {{"breakpoints", all_bps}, {"count", all_bps.size()}};
}

nlohmann::json get(const std::string& address_str) {
    auto& bridge = get_bridge();
    auto address = bridge.eval_expression(address_str);
    auto addr_hex = format_utils::format_address(address);
    for (auto type : {bp_normal, bp_hardware, bp_memory}) {
        auto bps = bridge.get_breakpoint_list(type);
        if (!bps)
            throw std::runtime_error(bps.error());
        for (auto& bp : bps.value()) {
            if (bp["address"] == addr_hex) {
                bp["label"] = bp["name"].get<std::string>().empty() ? bridge.get_label_at(address) : bp["name"].get<std::string>();
                return bp;
            }
        }
    }
    throw std::runtime_error("No breakpoint at " + address_str);
}

nlohmann::json set_software(const std::string& address, bool singleshot) {
    auto& bridge = get_bridge();
    if (!bridge.require_debugging()) throw std::runtime_error("No active debug session");
    if (!bridge.exec_command(singleshot ? "bp " + address + ", ss" : "bp " + address))
        throw std::runtime_error("Failed to set software breakpoint at " + address);
    return {{"address", address}, {"type", "software"}, {"singleshot", singleshot}};
}

nlohmann::json set_hardware(const std::string& address, const std::string& type, const std::string& size) {
    auto& bridge = get_bridge();
    if (!bridge.require_debugging()) throw std::runtime_error("No active debug session");
    if (!bridge.exec_command("bphws " + address + ", " + type + ", " + size))
        throw std::runtime_error("Failed to set hardware breakpoint at " + address);
    return {{"address", address}, {"type", "hardware"}, {"hw_type", type}, {"hw_size", size}};
}

nlohmann::json set_memory(const std::string& address, const std::string& type) {
    auto& bridge = get_bridge();
    if (!bridge.require_debugging()) throw std::runtime_error("No active debug session");
    if (!bridge.exec_command("bpm " + address + ", " + type))
        throw std::runtime_error("Failed to set memory breakpoint at " + address);
    return {{"address", address}, {"type", "memory"}, {"mem_type", type}};
}

nlohmann::json delete_bp(const std::string& address, const std::string& type) {
    auto& bridge = get_bridge();
    bool ok;
    if (type == "software") ok = bridge.exec_command("bc " + address);
    else if (type == "hardware") ok = bridge.exec_command("bphwc " + address);
    else if (type == "memory") ok = bridge.exec_command("bpmc " + address);
    else throw std::runtime_error("Invalid breakpoint type '" + type + "', expected: software, hardware, memory");
    if (!ok) throw std::runtime_error("Failed to delete breakpoint at " + address);
    return {{"address", address}, {"deleted", true}};
}

nlohmann::json enable(const std::string& address) {
    if (!get_bridge().exec_command("bpe " + address))
        throw std::runtime_error("Failed to enable breakpoint at " + address);
    return {{"address", address}, {"enabled", true}};
}

nlohmann::json disable(const std::string& address) {
    if (!get_bridge().exec_command("bpd " + address))
        throw std::runtime_error("Failed to disable breakpoint at " + address);
    return {{"address", address}, {"enabled", false}};
}

nlohmann::json toggle(const std::string& address) {
    auto& bridge = get_bridge();
    if (!bridge.require_debugging()) throw std::runtime_error("No active debug session");
    auto addr = bridge.eval_expression(address);
    // Try each breakpoint type to find the one at this address
    BRIDGEBP bp{};
    bool found = false;
    for (auto type : {bp_normal, bp_hardware, bp_memory}) {
        if (DbgFunctions()->GetBridgeBp(type, addr, &bp)) { found = true; break; }
    }
    if (!found) throw std::runtime_error("No breakpoint at " + address);
    bool new_enabled = !bp.enabled;
    if (!bridge.exec_command((new_enabled ? "bpe " : "bpd ") + address))
        throw std::runtime_error("Failed to toggle breakpoint at " + address);
    return {{"address", address}, {"enabled", new_enabled}};
}

nlohmann::json set_condition(const std::string& address, const std::string& condition) {
    auto& bridge = get_bridge();
    const auto result = bridge.set_breakpoint_text_field(
        bp_normal, bridge.eval_expression(address), bpf_breakcondition, condition);
    if (!result)
        throw std::runtime_error("Failed to set breakpoint condition at " + address
                                 + ": " + result.error());
    GuiUpdateBreakpointsView();
    return {{"address", address}, {"condition", condition}};
}

nlohmann::json set_log(const std::string& address, const std::string& text) {
    auto& bridge = get_bridge();
    const auto result = bridge.set_breakpoint_log_text(
        bp_normal, bridge.eval_expression(address), text);
    if (!result)
        throw std::runtime_error("Failed to set breakpoint log at " + address
                                 + ": " + result.error());
    GuiUpdateBreakpointsView();
    return {{"address", address}, {"log", text}};
}

nlohmann::json reset_hit_count(const std::string& address) {
    auto& bridge = get_bridge();
    const auto result = bridge.set_breakpoint_number_field(
        bp_normal, bridge.eval_expression(address), bpf_hitcount, 0);
    if (!result)
        throw std::runtime_error("Failed to reset hit count at " + address
                                 + ": " + result.error());
    GuiUpdateBreakpointsView();
    return {{"address", address}, {"hit_count", 0}};
}

nlohmann::json configure(const nlohmann::json& args) {
    auto& bridge = get_bridge();
    if (!bridge.require_debugging()) throw std::runtime_error("No active debug session");
    return configure_one(bridge, args);
}

nlohmann::json configure_batch(const nlohmann::json& breakpoints_array) {
    auto& bridge = get_bridge();
    if (!bridge.require_debugging()) throw std::runtime_error("No active debug session");
    auto results = nlohmann::json::array();
    int succeeded = 0, failed = 0;
    for (const auto& entry : breakpoints_array) {
        std::string address;
        bool has_address = false;
        try {
            if (entry.is_object()
                && entry.contains("address")
                && entry["address"].is_string()) {
                address = entry["address"].get<std::string>();
                has_address = true;
            }
            const auto configured = configure_one(bridge, entry);
            results.push_back({
                {"address", configured.at("address")},
                {"success", true}
            });
            ++succeeded;
        } catch (const std::exception& e) {
            nlohmann::json item = {
                {"error", e.what()},
                {"success", false}
            };
            if (has_address)
                item["address"] = address;
            results.push_back(std::move(item));
            ++failed;
        }
    }
    return {{"results", results}, {"total", breakpoints_array.size()}, {"succeeded", succeeded}, {"failed", failed}};
}

} // namespace handlers::breakpoints
