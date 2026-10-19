#include "krprof/attribution.hpp"

namespace krprof {
namespace {

constexpr bool is_sep(char c) noexcept { return c == '/' || c == '\\'; }

constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) { return false; }
    }
    return true;
}

// Слой скриптов: `1_Core`, `3_Game`, `4_World`, `5_Mission`. В одном и том же логе
// встречается и `4_World`, и `4_world`, поэтому смотрим на форму, а не на список имён:
// цифра, подчёркивание, дальше буквы.
constexpr bool is_layer(std::string_view seg) noexcept {
    if (seg.size() < 3) { return false; }
    if (seg[0] < '1' || seg[0] > '9') { return false; }
    if (seg[1] != '_') { return false; }
    for (std::size_t i = 2; i < seg.size(); ++i) {
        const char c = lower(seg[i]);
        if (c < 'a' || c > 'z') { return false; }
    }
    return true;
}

// Метка — первый сегмент, за которым начинается уже НУТРО мода, а не его префикс.
// Это либо сам `scripts`, либо сразу слой: часть модов кладёт слой прямо под префикс.
constexpr bool is_marker(std::string_view seg) noexcept {
    return iequals(seg, "scripts") || is_layer(seg);
}

// Путь без буквы диска и ведущих разделителей.
constexpr std::string_view normalize(std::string_view path) noexcept {
    if (path.size() >= 2 && path[1] == ':') { path.remove_prefix(2); }
    while (!path.empty() && is_sep(path.front())) { path.remove_prefix(1); }
    return path;
}

// Позиция метки в пути — байтовое смещение её начала, либо npos.
constexpr std::size_t marker_at(std::string_view path) noexcept {
    std::size_t at = 0;
    while (at < path.size()) {
        std::size_t end = path.find_first_of("/\\", at);
        if (end == std::string_view::npos) { return std::string_view::npos; }
        if (is_marker(path.substr(at, end - at))) { return at; }
        at = end + 1;
    }
    return std::string_view::npos;
}

}  // namespace

std::string_view prefix_of(std::string_view path) noexcept {
    path = normalize(path);
    const std::size_t at = marker_at(path);
    if (at == std::string_view::npos || at == 0) { return {}; }
    return path.substr(0, at - 1);  // без разделителя перед меткой
}

std::string_view mod_of(std::string_view path) noexcept {
    path = normalize(path);
    if (path.empty()) { return unknown_mod; }

    const std::size_t at = marker_at(path);

    // Метки нет — значит нет и префикса мода: так лежит ванильный init.c.
    // Метка первым сегментом — перед ней ничего, это тоже ваниль.
    if (at == std::string_view::npos || at == 0) { return vanilla_mod; }

    // Мод — ПОСЛЕДНИЙ сегмент перед меткой. Не первый: у `KR/diag/KR_GUARANA/scripts/...`
    // мод это KR_GUARANA, а `KR/diag` — лишь то, как он разложен в репозитории.
    const std::string_view head = path.substr(0, at - 1);
    const std::size_t sep = head.find_last_of("/\\");
    return sep == std::string_view::npos ? head : head.substr(sep + 1);
}

}  // namespace krprof
