#pragma once

#include <string_view>

namespace path_utils {

    [[nodiscard]] constexpr bool is_separator(char ch) {
        return ch == '\\' || ch == '/';
    }

    [[nodiscard]] constexpr bool is_ascii_alpha(char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    }

    [[nodiscard]] constexpr char ascii_upper(char ch) {
        return ch >= 'a' && ch <= 'z'
            ? static_cast<char>(ch - ('a' - 'A'))
            : ch;
    }

    [[nodiscard]] inline bool has_namespace_prefix(
        std::string_view path,
        char marker
    ) {
        return path.size() >= 4
            && is_separator(path[0])
            && is_separator(path[1])
            && path[2] == marker
            && is_separator(path[3]);
    }

    [[nodiscard]] inline bool is_unc_tail(
        std::string_view path,
        bool extended
    ) {
        const auto find_separator = [extended](std::string_view value) {
            return extended ? value.find('\\') : value.find_first_of("\\/");
        };

        const auto server_end = find_separator(path);
        if (server_end == std::string_view::npos || server_end == 0)
            return false;
        const auto share = path.substr(server_end + 1);
        const auto share_end = find_separator(share);
        return !share.empty() && share_end != 0;
    }

    // Accept drive-rooted paths, UNC paths, and extended-length drive/UNC paths.
    // Reject drive-relative paths (C:foo), rooted paths without a drive (\foo),
    // and Win32 device paths (\\.\..., including slash/mixed-slash aliases),
    // which are not log files.
    [[nodiscard]] inline bool is_absolute_windows_path(std::string_view path) {
        if (path.find('\0') != std::string_view::npos)
            return false;

        if (path.size() >= 3
            && is_ascii_alpha(path[0])
            && path[1] == ':' && is_separator(path[2])) {
            return true;
        }

        if (path.size() < 5 || !is_separator(path[0]) || !is_separator(path[1]))
            return false;

        // Win32 normalizes slash and mixed-slash forms such as //./C: to the
        // device namespace too, so inspect the prefix by separator semantics.
        if (has_namespace_prefix(path, '.'))
            return false;

        if (has_namespace_prefix(path, '?')) {
            // Extended paths bypass Win32 normalization. Accept only their
            // canonical backslash spelling and only drive/UNC file paths.
            if (path[0] != '\\' || path[1] != '\\' || path[3] != '\\')
                return false;

            const auto extended = path.substr(4);
            if (extended.size() >= 3
                && is_ascii_alpha(extended[0])
                && extended[1] == ':' && extended[2] == '\\') {
                return true;
            }
            if (extended.size() < 4
                || ascii_upper(extended[0]) != 'U'
                || ascii_upper(extended[1]) != 'N'
                || ascii_upper(extended[2]) != 'C'
                || extended[3] != '\\') {
                return false;
            }
            return is_unc_tail(extended.substr(4), true);
        }

        return is_unc_tail(path.substr(2), false);
    }

    [[nodiscard]] inline bool contains_embedded_nul(std::string_view path) {
        return path.find('\0') != std::string_view::npos;
    }

} // namespace path_utils
