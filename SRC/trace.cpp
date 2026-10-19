#include "krprof/trace.hpp"

#include <array>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "krprof/attribution.hpp"

namespace krprof {
namespace {

void jstr(std::string& out, std::string_view text) {
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

// Узел дерева вызовов в терминах Chrome: идентификатор, кадр и родитель.
struct cpu_node {
    int id = 0;
    int parent = 0;
    frame_id sym = 0;
    bool is_root = false;
};

}  // namespace

std::string to_chrome_trace(const sample_log& log,
                            const symbol_table& symbols,
                            const run_meta& meta) {
    // ── Почему именно CPU-профиль, а не отрезки B/E ──────────────────────────
    // Сначала здесь были парные события B/E — Perfetto их принимал, а Firefox Profiler
    // отказал: «A tracing event in the chrome profile did not follow the expected form».
    // Причина не в мелочи разметки: его импортёр Chrome-трасс ждёт от сэмплирующего
    // профайлера ИМЕННО CPU-профиль — дерево узлов плюс номера узлов по сэмплам. Это и
    // правильнее по сути: у нас снимки, а не отрезки, и отрезки были домыслом.
    //
    // Эту же форму читают Chrome DevTools, Perfetto и speedscope, так что один файл
    // открывается везде.
    std::vector<cpu_node> nodes;
    nodes.push_back(cpu_node{1, 0, 0, true});  // корень: Chrome нумерует узлы с единицы

    std::unordered_map<std::uint64_t, int> edge;  // (родитель, кадр) -> узел
    std::vector<int> at_sample;
    at_sample.reserve(log.size());

    for (std::size_t i = 0; i < log.size(); ++i) {
        int cur = 1;
        for (const frame_id f : log.stack_at(i)) {
            const std::uint64_t key = (static_cast<std::uint64_t>(cur) << 32) | f;
            const auto [it, fresh] = edge.try_emplace(key, static_cast<int>(nodes.size()) + 1);
            if (fresh) { nodes.push_back(cpu_node{it->second, cur, f, false}); }
            cur = it->second;
        }
        at_sample.push_back(cur);
    }

    const std::uint64_t start = log.size() != 0 ? log.time_at(0) : 0;
    const std::uint64_t end = log.size() != 0 ? log.time_at(log.size() - 1) : 0;

    std::string o;
    o.reserve(1u << 20);
    o.append("{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n");

    // Имена дорожки: без них просмотрщик подпишет её «Thread 1».
    o.append(R"({"cat":"__metadata","name":"process_name","ph":"M","pid":1,"tid":1,)"
             R"("ts":0,"args":{"name":"DayZ server"}},)"
             "\n");
    o.append(R"({"cat":"__metadata","name":"thread_name","ph":"M","pid":1,"tid":1,)"
             R"("ts":0,"args":{"name":"Script"}},)"
             "\n");

    // Открывающее событие профиля.
    o.append(R"({"cat":"disabled-by-default-v8.cpu_profiler","name":"Profile","ph":"P",)"
             R"("pid":1,"tid":1,"id":"0x1","ts":)");
    o.append(std::to_string(start));
    o.append(R"(,"args":{"data":{"startTime":)").append(std::to_string(start));
    o.append("}}},\n");

    // Сам профиль: дерево узлов, номера узлов по сэмплам и промежутки между ними.
    o.append(R"({"cat":"disabled-by-default-v8.cpu_profiler","name":"ProfileChunk",)"
             R"("ph":"P","pid":1,"tid":1,"id":"0x1","ts":)");
    o.append(std::to_string(end));
    o.append(R"(,"args":{"data":{"cpuProfile":{"nodes":[)");

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const cpu_node& n = nodes[i];
        if (i != 0) { o.push_back(','); }
        o.append("{\"id\":").append(std::to_string(n.id));
        o.append(",\"callFrame\":{\"functionName\":\"");
        if (n.is_root) {
            o.append("(root)");
        } else {
            const symbol& s = symbols[n.sym];
            if (!s.klass.empty()) {
                jstr(o, s.klass);
                o.append("::");
            }
            jstr(o, s.func.empty() ? std::string_view{"?"} : std::string_view{s.func});
        }
        // `url` — то, что просмотрщик показывает как источник. Путь скрипта начинается
        // с префикса мода, поэтому мод виден прямо здесь и отдельного поля не просит.
        o.append("\",\"scriptId\":\"0\",\"url\":\"");
        if (!n.is_root) { jstr(o, symbols[n.sym].file); }
        o.append("\",\"lineNumber\":");
        // Chrome считает строки с нуля, движок — с единицы.
        o.append(std::to_string(n.is_root || symbols[n.sym].line == 0
                                    ? 0
                                    : static_cast<int>(symbols[n.sym].line) - 1));
        o.append(",\"columnNumber\":0}");
        if (!n.is_root) { o.append(",\"parent\":").append(std::to_string(n.parent)); }
        o.append("}");
    }

    o.append("],\"startTime\":").append(std::to_string(start));
    o.append(",\"endTime\":").append(std::to_string(end));
    o.append(",\"samples\":[");
    for (std::size_t i = 0; i < at_sample.size(); ++i) {
        if (i != 0) { o.push_back(','); }
        o.append(std::to_string(at_sample[i]));
    }
    o.append("]},\"timeDeltas\":[");
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (i != 0) { o.push_back(','); }
        // Первый промежуток нулевой: профиль начинается ровно с первого снимка.
        o.append(std::to_string(i == 0 ? 0 : log.time_at(i) - log.time_at(i - 1)));
    }
    o.append("]}}}\n]}\n");
    return o;
}

}  // namespace krprof
