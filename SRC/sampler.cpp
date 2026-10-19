#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <graft/mt.hpp>

#include "krprof/attribution.hpp"
#include "krprof/call_tree.hpp"
#include "krprof/engine.hpp"
#include "krprof/folded.hpp"
#include "krprof/html.hpp"
#include "krprof/modmap.hpp"
#include "krprof/native_stack.hpp"
#include "krprof/sample_log.hpp"
#include "krprof/sampler.hpp"
#include "krprof/sidecar.hpp"
#include "krprof/speedscope.hpp"
#include "krprof/symbols.hpp"
#include "krprof/trace.hpp"

namespace krprof::sampler {
namespace {

call_tree g_tree;   // агрегат: свёрнутые стеки и сводка по модам
sample_log g_log;   // порядок времени: speedscope и покадровый разбор
symbol_table g_symbols;
run_meta g_meta;
std::atomic<bool> g_stop{false};

// Каденция держится СПИНОМ, а не sleep: гранулярность сна у Windows ~15 мс, и на 500 Гц
// сэмплер со `sleep_for` снимал бы шестьдесят кадров в секунду вместо пятисот.
void spin_until(std::chrono::steady_clock::time_point when) {
    while (std::chrono::steady_clock::now() < when) { YieldProcessor(); }
}

// Ключ мемоизации: пара <функция, класс объекта>. Именно пара, а не один `Funct*`: один
// и тот же метод базового класса исполняется на объектах разных классов, и в флеймграфе
// это разные кадры.
// Ключ мемоизации НЕ включает `stat`: строка внутри функции меняется от снимка к снимку,
// а кадр в флеймграфе должен оставаться одним. Файл и мод от строки не зависят, поэтому
// разбор по-прежнему делается один раз на пару <функция, класс>.
std::uint64_t key_of(const engine::raw_frame& f) {
    const auto a = reinterpret_cast<std::uintptr_t>(f.funct);
    const auto b = reinterpret_cast<std::uintptr_t>(f.object);
    return a ^ (b * 0x9E3779B97F4A7C15ull);
}

frame_id resolve(const engine::raw_frame& f) {
    return g_symbols.intern(key_of(f), [&] {
        symbol s;
        s.func = engine::funct_name(f.funct);
        s.klass = engine::class_name(f.object);

        // Мод — из отладочной информации движка. Именно здесь различаются пять вложенных
        // DayZGame::OnUpdate: класс у них один, а файл у каждого свой.
        const engine::source_ref src = engine::source_of_frame(f.funct, f.stat);
        s.file = src.file;
        s.line = src.line;

        if (!src.file.empty()) {
            s.kind = frame_kind::script;
            s.mod = std::string{mod_of(src.file)};
        } else if (engine::is_native(f.funct)) {
            // Не провал разбора, а честный ответ: у натива исходника нет, он объявлен
            // в C++. Отделяем от движкового C++: это ГРАНИЦА миров, и по ней видно,
            // сколько скрипт тратит на обращения к движку.
            s.kind = frame_kind::proto;
            s.mod = std::string{proto_mod};
        } else {
            s.kind = frame_kind::unknown;
            s.mod = std::string{unknown_mod};
        }
        return s;
    });
}

// Кадр движкового C++ из стека ОС. Ключ — НАЧАЛО функции, а не адрес возврата: иначе
// одна функция рассыпалась бы на десяток строк по разным точкам вызова.
frame_id resolve_native(void* ret_addr) {
    void* start = native::function_start(ret_addr);
    return g_symbols.intern(reinterpret_cast<std::uint64_t>(start) | (1ull << 63), [&] {
        symbol s;
        s.kind = frame_kind::engine;
        s.mod = std::string{engine_mod};
        s.func = native::name_of(start);
        return s;
    });
}

std::string stamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u_%02u-%02u-%02u",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

bool write_file(const std::string& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) { return false; }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

}  // namespace

void run(unsigned hz, unsigned seconds, log_fn say) {
    void* thread = engine::open_game_thread();
    if (thread == nullptr) {
        say("поток игрового цикла не опознан — сэмплировать некого");
        return;
    }

    if (!engine::probe(thread, say)) {
        CloseHandle(static_cast<HANDLE>(thread));
        return;
    }
    // Отладочная информация не обязательна для работы: без неё профиль всё равно
    // строится, просто моды уедут в `$unknown`/`$engine`. Поэтому отказ здесь — не выход.
    engine::probe_debug(say);

    say(std::format("сэмплирую {} с на {} Гц", seconds, hz));

    const auto period = std::chrono::nanoseconds(1'000'000'000ull / hz);
    const auto started = std::chrono::steady_clock::now();
    const auto until = started + std::chrono::seconds(seconds);
    auto next = started + period;

    std::vector<engine::raw_frame> raw;
    std::vector<void*> os_raw;
    native::snapshot os_snap;
    std::vector<frame_id> stack;
    std::uint64_t quiet = 0;
    g_meta.sample_hz = hz;

    // ── Покадровое ───────────────────────────────────────────────────────────
    // Кадр опознаётся как ЭПИЗОД ИСПОЛНЕНИЯ: стек скрипта был пуст — стал непустым.
    // Между кадрами движок занят не скриптом, поэтому стек там пуст.
    bool idle = true;
    std::uint64_t episode_from = 0;
    std::vector<std::uint64_t> episodes;

    std::uint64_t dropped_depth[32] = {};
    std::uint64_t dropped_frames_seen = 0;
    std::uint64_t dropped_null_funct = 0;

    while (!g_stop.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < until) {
        spin_until(next);
        next += period;

        const auto now = std::chrono::steady_clock::now();
        const auto at_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - started).count());

        const std::size_t depth = engine::capture(thread, raw);
        if (depth == 0) {
            // Скрипт молчит — а движок работает, и это две трети времени сервера. Раньше
            // тут была полная слепота; теперь снимаем стек ОС. Выборочно: раскрутка
            // требует контекста потока и потому втрое дороже обычного снимка.
            // Снимок берётся ПОД приостановкой (только копирование), разбор — после
            // возобновления. Иначе разматывали бы живой стек и роняли сервер: так уже
            // было, см. комментарий в native_stack.cpp.
            if ((++quiet & 7) == 0 && engine::capture_native(thread, os_snap) &&
                native::walk(os_snap, os_raw, 32) != 0) {
                stack.clear();
                // Стек ОС идёт изнутри наружу — переворачиваем, чтобы корень был первым,
                // как и у скриптового.
                for (auto it = os_raw.rbegin(); it != os_raw.rend(); ++it) {
                    stack.push_back(resolve_native(*it));
                }
                if (!stack.empty()) {
                    g_tree.add(stack);
                    g_log.add(stack, at_us);
                    ++g_meta.samples;
                    ++g_meta.engine_samples;
                }
            }
            // Скрипт сейчас не исполняется — это не потеря, это простой между кадрами.
            // Считать такие снимки «дропами» значило бы врать в метаданных.
            if (!idle) { episodes.push_back(at_us - episode_from); }
            idle = true;
            continue;
        }
        if (idle) {
            episode_from = at_us;
            idle = false;
        }

        // Символизация СНАРУЖИ окна остановки: поток уже отпущен.
        stack.clear();
        for (const engine::raw_frame& f : raw) {
            if (f.funct == nullptr) { continue; }
            stack.push_back(resolve(f));
        }
        if (stack.empty()) {
            // Диагностика: у снимка была глубина, но НИ ОДИН кадр не дал функции.
            // Считаем распределение по глубине — если дыра приходится на конкретную
            // глубину, это раскладка, а если размазана, то стек чужой структуры.
            ++g_meta.dropped;
            if (depth < 32) { ++dropped_depth[depth]; }
            ++dropped_frames_seen;
            for (const engine::raw_frame& f : raw) {
                if (f.funct == nullptr) { ++dropped_null_funct; }
            }
            continue;
        }
        g_tree.add(stack);
        g_log.add(stack, at_us);
        ++g_meta.samples;
    }

    g_meta.wall_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());

    g_meta.frames = episodes.size();
    if (!episodes.empty()) {
        std::sort(episodes.begin(), episodes.end());
        g_meta.median_frame_us = episodes[episodes.size() / 2];
        g_meta.worst_frame_us = episodes.back();
    }

    if (g_meta.dropped != 0) {
        std::string hist;
        for (unsigned d = 0; d < 32; ++d) {
            if (dropped_depth[d] != 0) {
                hist += std::format(" гл{}={}", d, dropped_depth[d]);
            }
        }
        say(std::format("потерянные снимки: {} шт, кадров без функции {}, глубины:{}",
                        dropped_frames_seen, dropped_null_funct, hist));
    }

    CloseHandle(static_cast<HANDLE>(thread));
    dump(say);
}

void stop() { g_stop.store(true, std::memory_order_relaxed); }

void dump(log_fn say) {
    if (g_tree.total() == 0) {
        say("снимков нет — выгружать нечего");
        return;
    }

    // Имя мода уточняется ЗДЕСЬ, а не на символизации: отличить подкаталог мода от его
    // префикса можно только по всему набору путей сразу (KR_CORE/MATH, /CORO, /EVENT —
    // это один мод, а не три). На съёме набор ещё неполон.
    mod_map map;
    g_symbols.for_each([&](symbol& s) {
        if (!s.file.empty()) { map.observe(s.file); }
    });
    map.finish();
    g_symbols.for_each([&](symbol& s) {
        if (!s.file.empty()) { s.mod = std::string{map.of(s.file)}; }
    });

    const std::string base = "krprof_" + stamp();
    // speedscope — основной: он хранит порядок времени, поэтому даёт и флеймграф, и
    // покадровый разбор. Свёрнутые стеки идут рядом для inferno/flamegraph.pl.
    // Chrome Trace — основной вывод: его читают Perfetto, Firefox Profiler и Chrome
    // DevTools, то есть готовые популярные просмотрщики, которые не надо ставить.
    const bool t = write_file(base + ".trace.json",
                              to_chrome_trace(g_log, g_symbols, g_meta));
    const bool a =
        write_file(base + ".speedscope.json", to_speedscope_json(g_log, g_symbols, g_meta));
    const bool b = write_file(base + ".folded", to_folded(g_tree, g_symbols));
    const bool c = write_file(base + ".json", to_sidecar_json(g_tree, g_symbols, g_meta));
    // Страница-отчёт: то, что читают глазами. Флеймграф показывает форму стека, а
    // вопрос обычно про цену — её проще прочитать таблицей.
    const bool d = write_file(base + ".html", to_html(g_tree, g_log, g_symbols, g_meta));

    say(std::format("снимков {}, узлов {}, символов {}, потеряно {}",
                    g_meta.samples, g_tree.nodes(), g_symbols.size(), g_meta.dropped));
    say(std::format("кадров {} (медиана {} мкс, худший {} мкс)",
                    g_meta.frames, g_meta.median_frame_us, g_meta.worst_frame_us));

    // Куда делись остальные снимки. Отдельной строкой, потому что «спал» и «не было
    // потока» — разные вещи: первое означает корутины в ожидании, второе простой движка.
    const engine::skip_stats sk = engine::skips();
    say(std::format("пропущено: потока нет {}, поток спал {}, стек пуст {}",
                    sk.no_thread, sk.not_running, sk.empty));
    say(std::format("стек ОС: попыток {}, снято {}, кадров движка в профиле {}",
                    sk.native_tries, sk.native_ok, g_meta.engine_samples));
    say(std::format("целостность: порвано {} снимков; негодных кадров {} из {} "
                    "(вершина {}, основание {}, середина {})",
                    sk.torn, sk.bad_frames, sk.checked_frames,
                    sk.bad_at_top, sk.bad_at_bottom, sk.bad_middle));
    say(std::format("состояния потока: 1(идёт,не усыпить)={} 2(идёт)={} 3(Sleep)={} "
                    "4(Wait)={} 5(разобран)={}",
                    sk.by_state[1], sk.by_state[2], sk.by_state[3], sk.by_state[4],
                    sk.by_state[5]));

    // Цена профайлера для сервера, померенная на его же потоке.
    const engine::window_stats w = engine::windows();
    if (w.count != 0 && g_meta.wall_ms != 0) {
        const double share = 100.0 * static_cast<double>(w.total_us) /
                             (static_cast<double>(g_meta.wall_ms) * 1000.0);
        say(std::format("окно остановки: медиана {} мкс, p99 {} мкс, макс {} мкс; "
                        "остановок {}, суммарно {} мкс = {:.3f}% времени сервера",
                        w.median_us, w.p99_us, w.max_us, w.count, w.total_us, share));
    }
    say(std::format("выгружено {}: trace={} speedscope={} html={} folded={} json={}",
                    base, t, a, d, b, c));
}

}  // namespace krprof::sampler
