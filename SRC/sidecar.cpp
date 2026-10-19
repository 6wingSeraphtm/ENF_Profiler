#include "krprof/sidecar.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <unordered_map>

#include "krprof/attribution.hpp"

namespace krprof {
namespace {

// Имена приходят из чужой памяти движка. Кавычка или обратный слэш внутри имени делают
// сайдкар невалидным JSON, и отчёт МОЛЧА перестаёт открываться — поэтому экранируем всё,
// что положено, включая управляющие символы.
void append_escaped(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::array<char, 8> buf{};
                    std::snprintf(buf.data(), buf.size(), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out.append(buf.data());
                } else {
                    out.push_back(c);
                }
        }
    }
}

std::string percent(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) { return "0.00"; }
    std::array<char, 16> buf{};
    const double share = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    std::snprintf(buf.data(), buf.size(), "%.2f", share);
    return buf.data();
}

}  // namespace

std::string to_sidecar_json(const call_tree& tree,
                            const symbol_table& symbols,
                            const run_meta& meta) {
    // Сводка по СОБСТВЕННОМУ весу: время, ушедшее в super или в чужой вызов, принадлежит
    // тому, кто его исполнял. Иначе мод из одной строки `super.OnUpdate()` оказался бы
    // самым дорогим в игре — он ведь «покрывает» всё, что под ним.
    std::unordered_map<std::string_view, std::uint64_t> by_mod;
    for (const folded_entry& entry : tree.flatten()) {
        if (entry.path.empty()) { continue; }
        const symbol& leaf = symbols[entry.path.back()];
        const std::string_view mod = leaf.mod.empty() ? unknown_mod : std::string_view{leaf.mod};
        by_mod[mod] += entry.self;
    }

    std::vector<std::pair<std::string_view, std::uint64_t>> mods(by_mod.begin(), by_mod.end());
    // Дороже — выше: самый дорогой мод читают первым. Имя вторым ключом, чтобы порядок
    // не зависел от обхода хеш-таблицы при равных весах.
    std::ranges::sort(mods, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });

    std::string out = "{\n  \"krprof\": 1,\n  \"meta\": {";
    out.append("\n    \"samples\": ").append(std::to_string(meta.samples));
    out.append(",\n    \"sample_hz\": ").append(std::to_string(meta.sample_hz));
    out.append(",\n    \"wall_ms\": ").append(std::to_string(meta.wall_ms));
    out.append(",\n    \"dropped\": ").append(std::to_string(meta.dropped));
    out.append("\n  },\n  \"mods\": ");

    if (mods.empty()) {
        out.append("[]\n}\n");
        return out;
    }

    out.append("[\n");
    for (std::size_t i = 0; i < mods.size(); ++i) {
        out.append("    { \"mod\": \"");
        append_escaped(out, mods[i].first);
        out.append("\", \"self\": ").append(std::to_string(mods[i].second));
        out.append(", \"share\": ").append(percent(mods[i].second, tree.total()));
        out.append(i + 1 == mods.size() ? " }\n" : " },\n");
    }
    out.append("  ]\n}\n");
    return out;
}

}  // namespace krprof
