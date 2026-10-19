#include "krprof/speedscope.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <unordered_map>

#include "krprof/attribution.hpp"

namespace krprof {
namespace {

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

// Имя кадра — то же, что в свёрнутых стеках: `мод|Класс::Функция`. Один формат имени на
// оба вывода, чтобы отчёты можно было класть рядом и сличать глазами.
void append_frame_name(std::string& out, const symbol& s) {
    append_escaped(out, s.mod.empty() ? unknown_mod : std::string_view{s.mod});
    out.push_back('|');
    if (!s.klass.empty()) {
        append_escaped(out, s.klass);
        out.append("::");
    }
    if (s.func.empty()) {
        out.push_back('?');
    } else {
        append_escaped(out, s.func);
    }
}

}  // namespace

std::string to_speedscope_json(const sample_log& log,
                               const symbol_table& symbols,
                               const run_meta& meta) {
    // Период сэмплирования: им ограничивается вес и им же добирается последний снимок.
    const std::uint64_t period = meta.sample_hz != 0 ? 1'000'000ull / meta.sample_hz : 1000ull;
    // Провал в снимках означает, что скрипт не исполнялся вовсе. Приписать эту паузу
    // последнему кадру нельзя — один снимок перед простоем съел бы весь флеймграф.
    const std::uint64_t cap = period * 4;

    // Таблица кадров общая: один и тот же кадр во многих снимках — одна запись и один
    // индекс. Своя нумерация нужна потому, что `frame_id` разрежен, а speedscope ждёт
    // плотный индекс в массив.
    std::unordered_map<frame_id, std::uint32_t> index;
    std::vector<frame_id> order;
    for (std::size_t i = 0; i < log.size(); ++i) {
        for (const frame_id id : log.stack_at(i)) {
            if (index.try_emplace(id, static_cast<std::uint32_t>(order.size())).second) {
                order.push_back(id);
            }
        }
    }

    std::string out = "{\n  \"$schema\": \"https://www.speedscope.app/file-format-schema.json\",";
    out.append("\n  \"exporter\": \"krprof 1\",");
    out.append("\n  \"shared\": { \"frames\": [");
    for (std::size_t i = 0; i < order.size(); ++i) {
        const symbol& s = symbols[order[i]];
        out.append(i == 0 ? "\n    " : ",\n    ");
        out.append("{ \"name\": \"");
        append_frame_name(out, s);
        out.push_back('"');
        if (!s.file.empty()) {
            out.append(", \"file\": \"");
            append_escaped(out, s.file);
            out.push_back('"');
        }
        if (s.line != 0) { out.append(", \"line\": ").append(std::to_string(s.line)); }
        out.append(" }");
    }
    out.append(order.empty() ? "] },\n" : "\n  ] },\n");

    // Веса: интервал до следующего снимка, а не «один снимок — одна единица». С ним ось
    // времени настоящая, и всплеск в 200 мс выглядит на 200 мс.
    std::vector<std::uint64_t> weights(log.size());
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const std::uint64_t w =
            i + 1 < log.size() ? std::min(log.time_at(i + 1) - log.time_at(i), cap) : period;
        weights[i] = w;
        total += w;
    }

    out.append("  \"profiles\": [\n    {\n      \"type\": \"sampled\",");
    out.append("\n      \"name\": \"DayZ script\",");
    out.append("\n      \"unit\": \"microseconds\",");
    out.append("\n      \"startValue\": 0,");
    out.append("\n      \"endValue\": ").append(std::to_string(total)).append(",");

    out.append("\n      \"samples\": [");
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (i != 0) { out.push_back(','); }
        out.push_back('[');
        const auto stack = log.stack_at(i);
        for (std::size_t k = 0; k < stack.size(); ++k) {
            if (k != 0) { out.push_back(','); }
            out.append(std::to_string(index[stack[k]]));
        }
        out.push_back(']');
    }
    out.append("],");

    out.append("\n      \"weights\": [");
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (i != 0) { out.push_back(','); }
        out.append(std::to_string(weights[i]));
    }
    out.append("]\n    }\n  ]\n}\n");
    return out;
}

}  // namespace krprof
