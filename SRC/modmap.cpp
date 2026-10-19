#include "krprof/modmap.hpp"

namespace krprof {
namespace {

// Родитель префикса: `KR/core/KR_CORE/MATH` -> `KR/core/KR_CORE`.
std::string_view parent_of(std::string_view prefix) {
    const std::size_t sep = prefix.find_last_of("/\\");
    return sep == std::string_view::npos ? std::string_view{} : prefix.substr(0, sep);
}

std::string_view last_segment(std::string_view prefix) {
    const std::size_t sep = prefix.find_last_of("/\\");
    return sep == std::string_view::npos ? prefix : prefix.substr(sep + 1);
}

// Есть ли в пути явный сегмент `scripts`. Если есть — граница мода сказана явно, и
// схлопывать такой мод в родителя нельзя: `JM/CF/Scripts/...` это мод CF, а не JM.
bool has_scripts_marker(std::string_view path) {
    std::size_t at = 0;
    while (at < path.size()) {
        const std::size_t end = path.find_first_of("/\\", at);
        if (end == std::string_view::npos) { return false; }
        std::string_view seg = path.substr(at, end - at);
        if (seg.size() == 7) {
            bool same = true;
            const char* want = "scripts";
            for (std::size_t i = 0; i < 7; ++i) {
                const char c = seg[i] >= 'A' && seg[i] <= 'Z' ? static_cast<char>(seg[i] + 32)
                                                              : seg[i];
                if (c != want[i]) { same = false; break; }
            }
            if (same) { return true; }
        }
        at = end + 1;
    }
    return false;
}

}  // namespace

void mod_map::observe(std::string_view path) {
    const std::string_view mod = mod_of(path);
    if (mod == vanilla_mod || mod == unknown_mod) { return; }
    // Явная граница — схлопывать нечего.
    if (has_scripts_marker(path)) { return; }

    const std::string_view prefix = prefix_of(path);
    const std::string_view parent = parent_of(prefix);
    if (parent.empty()) { return; }

    children_[std::string{parent}].emplace(mod);
}

void mod_map::finish() {
    for (const auto& [parent, kids] : children_) {
        // Один ребёнок — не повод схлопывать: может, мод и правда так называется.
        // Двое и больше разных имён под общим родителем — это части одного мода.
        if (kids.size() < 2) { continue; }
        const std::string name{last_segment(parent)};
        if (name.empty()) { continue; }
        for (const std::string& kid : kids) { collapse_[kid] = name; }
    }
}

std::string_view mod_map::of(std::string_view path) const {
    const std::string_view mod = mod_of(path);
    const auto it = collapse_.find(std::string{mod});
    return it == collapse_.end() ? mod : std::string_view{it->second};
}

}  // namespace krprof
