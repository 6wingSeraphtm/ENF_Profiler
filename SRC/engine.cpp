#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <vector>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>

#include <graft/mt.hpp>
#include <graft/native.hpp>
#include <graft/script.hpp>

#include "engine_mem.hpp"
#include "krprof/engine.hpp"

namespace krprof::engine {
namespace {

layout g_layout{};
void* g_sample = nullptr;
std::vector<raw_frame> g_samples;

// Каждое окно остановки — время, отнятое у сервера. Копим все и считаем распределение:
// среднее здесь врёт, потому что хвост длинный (планировщик), а решает как раз он.
std::vector<std::uint32_t> g_windows;
skip_stats g_skips;
std::int64_t g_qpc_freq = 0;

using namespace krprof::engine::mem;

// ── Раскладка `Funct`: имя ───────────────────────────────────────────────────
// Ищем не догадкой, а по ЯКОРЮ: берём метод, чьё имя знаем наверняка, и смотрим, по
// какому смещению в его дескрипторе лежит указатель ровно на эту строку. Сверяем на
// нескольких якорях — совпасть случайно на всех сразу указатель не может.
unsigned name_offset_from(void* desc, std::string_view name) {
    for (unsigned off = 0; off + sizeof(void*) <= 256; off += 8) {
        void* candidate = nullptr;
        if (read_at(desc, off, candidate) && string_is(candidate, name)) { return off; }
    }
    return ~0u;
}

struct anchor {
    const char* klass;
    const char* method;
};

constexpr anchor k_anchors[] = {
    {"ScriptModule", "Call"},
    {"ScriptModule", "CallFunction"},
    {"Managed", "Type"},
};

bool probe_funct(log_fn say) {
    unsigned agreed = ~0u;
    int votes = 0;

    for (const anchor& a : k_anchors) {
        const graft::script::method m = graft::script::find_method(a.klass, a.method);
        if (m.desc == nullptr) { continue; }

        const unsigned off = name_offset_from(m.desc, a.method);
        if (off == ~0u) { continue; }

        if (votes == 0) {
            agreed = off;
        } else if (off != agreed) {
            say(std::format("раскладка Funct не сошлась: +0x{:X} против +0x{:X}", agreed, off));
            return false;
        }
        ++votes;
    }

    if (votes < 2) {
        say("якорей для раскладки Funct не хватило");
        return false;
    }
    g_layout.funct_name = agreed;
    say(std::format("Funct::Name +0x{:X} (сошлось на {} якорях)", agreed, votes));
    return true;
}

// Валиден ли указатель как `Funct`: по нему читается разумное имя.
bool is_funct(const void* p) {
    if (p == nullptr) { return false; }
    void* name = nullptr;
    return read_at(p, g_layout.funct_name, name) && looks_like_name(name);
}

// Валиден ли указатель как объект: по нему читается дескриптор класса с именем.
// Смещения — из graft::layout, они уже сняты зондом и закреплены его сьютой.
bool is_object(const void* p) {
    if (p == nullptr) { return false; }
    void* klass = nullptr;
    if (!read_at(p, graft::layout::object_class, klass)) { return false; }
    void* name = nullptr;
    return read_at(klass, graft::layout::class_name, name) && looks_like_name(name);
}

}  // namespace

const layout& found() noexcept { return g_layout; }

void* probe_sample() noexcept { return g_sample; }

const std::vector<raw_frame>& probe_samples() noexcept { return g_samples; }

bool capture_native(void* thread, native::snapshot& out) {
    if (thread == nullptr) { return false; }
    if (SuspendThread(static_cast<HANDLE>(thread)) == static_cast<DWORD>(-1)) { return false; }
    const std::size_t got = native::capture_stack(thread, out);
    ResumeThread(static_cast<HANDLE>(thread));
    ++g_skips.native_tries;
    if (got != 0) { ++g_skips.native_ok; }
    return got != 0;
}

skip_stats skips() { return g_skips; }

window_stats windows() {
    window_stats w;
    if (g_windows.empty()) { return w; }

    std::vector<std::uint32_t> sorted = g_windows;
    std::sort(sorted.begin(), sorted.end());

    w.count = sorted.size();
    for (const std::uint32_t v : sorted) { w.total_us += v; }
    w.median_us = sorted[sorted.size() / 2];
    w.p99_us = sorted[sorted.size() * 99 / 100];
    w.max_us = sorted.back();
    return w;
}

namespace detail {
layout& mutable_layout() noexcept { return g_layout; }
}  // namespace detail

void* open_game_thread() {
    const unsigned tid = graft::mt::detail::game_thread_id();
    if (tid == 0) { return nullptr; }
    return OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                      FALSE,
                      tid);
}

std::size_t capture(void* thread, std::vector<raw_frame>& out) {
    out.clear();
    if (thread == nullptr || !graft::mt::available()) { return 0; }

    const graft::mt::machine& m = graft::mt::vm();

    // ── ОКНО ОСТАНОВКИ. Всё внутри — только копирование ──────────────────────
    // Здесь стоит игровой цикл сервера. Любая лишняя работа тут оплачивается кадром,
    // поэтому символизация, аллокации и журнал живут СНАРУЖИ.
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    if (SuspendThread(static_cast<HANDLE>(thread)) == static_cast<DWORD>(-1)) { return 0; }

    // `SuspendThread` асинхронна, и документация советует дожать её `GetThreadContext`.
    // ЗАМЕРЕНО, что здесь это не нужно: с ним медиана окна выросла с 2 до 12 мкс
    // (0.2 % -> 1.66 % времени сервера), а целостность не изменилась — рвущихся снимков
    // не было ни с ним, ни без него. Проверку даёт двойное чтение глубины ниже, и она
    // стоит два чтения вместо системного вызова.

    std::size_t depth = 0;
    graft::mt::script_thread* th = nullptr;
    static thread_local std::vector<raw_frame> scratch;
    scratch.clear();

    do {
        th = graft::mt::context_view{graft::mt::context()}.current();
        if (th == nullptr) {
            ++g_skips.no_thread;
            break;
        }

        // СОСТОЯНИЕ ОБЯЗАТЕЛЬНО. Опкод усыпления (Sleep/Wait) ставит потоку состояние
        // 3 или 4 и кладёт его в очередь ожидающих, но КАДРЫ ОСТАВЛЯЕТ ЖИВЫМИ, а
        // `ctx.current` продолжает на него смотреть. Без этой проверки спящая корутина
        // выглядела бы исполняющейся, и её время уехало бы в флеймграф как настоящее —
        // ровно там, где профайлеру верят больше всего.
        unsigned st = 0;
        if (!read_at(th, m.state, st)) { break; }
        if (st < 8) { ++g_skips.by_state[st]; }

        // ИСПОЛНЯЮТСЯ ДВА состояния, а не одно. `graft::mt::thread_state` перечисляет
        // только 2 (running), но у движка есть ещё 1 — TF_NONWAITABLE: «исполняется,
        // усыпить нельзя». Через него идёт ОБЫЧНЫЙ вызов из движка, то есть почти весь
        // скрипт сервера. Первая версия фильтра приняла 1 за «спит» и выбросила 30054
        // снимка из 30400 — профиль остался пустым. Проверено на живом сервере.
        constexpr unsigned k_nonwaitable = 1;
        const unsigned k_running = static_cast<unsigned>(graft::mt::thread_state::running);
        if (st != k_nonwaitable && st != k_running) {
            ++g_skips.not_running;
            break;
        }

        unsigned top = 0;
        if (!read_at(th, m.frame_index, top)) { break; }
        if (top == 0 || top >= m.frame_count) {
            ++g_skips.empty;
            break;
        }
        depth = top;

        // КАДРЫ 1-BASED. `frame_index` — это ИНДЕКС текущего кадра, а не их количество,
        // и нулевой кадр не используется вовсе: движок в GetCallStack идёт
        // `for (sp = stackPos; sp >= 1; sp--)`, то есть по [1..stackPos] включительно.
        //
        // Цена ошибки была велика и незаметна: цикл по [0..depth) ТЕРЯЛ самый внутренний
        // кадр (тот, что реально исполняется) и добавлял нулевой — а там лежит мусор от
        // прошлого вызова, который выглядит правдоподобно. Флеймграф был сдвинут на
        // кадр, и корнем всюду оказывался чужой остаток. Поймано на корутинах: у них
        // глубина 1, нулевой кадр пуст, и все снимки уходили в потери.
        const auto* frames = reinterpret_cast<const std::byte*>(th) + m.frames;
        scratch.resize(depth);
        for (std::size_t i = 1; i <= depth; ++i) {
            const void* frame = frames + static_cast<std::size_t>(m.frame_size) * i;
            raw_frame rf{};
            if (g_layout.frame_stat != ~0u) { read_at(frame, g_layout.frame_stat, rf.stat); }
            if (g_layout.frame_func != ~0u) { read_at(frame, g_layout.frame_func, rf.funct); }
            if (g_layout.frame_object != ~0u) { read_at(frame, g_layout.frame_object, rf.object); }
            scratch[i - 1] = rf;
        }
    } while (false);

    // Глубина ПОСЛЕ копирования. Если она уехала, поток не стоял, и в буфере смесь
    // двух моментов — такой снимок годится только на выброс.
    unsigned depth_after = ~0u;
    if (th != nullptr) { read_at(th, m.frame_index, depth_after); }

    ResumeThread(static_cast<HANDLE>(thread));
    // ── конец окна ───────────────────────────────────────────────────────────

    if (depth != 0 && depth_after != depth) {
        ++g_skips.torn;
        out.clear();
        return 0;
    }

    // Проверка целостности — СНАРУЖИ окна, на копии, и ВЫБОРОЧНО.
    //
    // Выборочно не ради экономии как таковой: `is_funct` ходит через `readable`, а тот
    // держит маленький кэш регионов. Проверка всех кадров подряд вымывала кэш, и
    // следующий снимок делал VirtualQuery уже ВНУТРИ окна остановки — цена профайлера
    // подскакивала с 0.2 % до 1.2 %. Диагностике хватает каждого шестьдесят четвёртого.
    // ВЕРШИНУ проверяем всегда. Замер показал, что негодные кадры бывают только на ней
    // (7 из 7 за прогон) — это кадр, который движок как раз заполняет: индекс уже
    // сдвинут, указатель функции ещё нет. Цена ошибки здесь наибольшая: лист несёт
    // СОБСТВЕННОЕ время, и мусорный лист крадёт его у настоящего.
    if (!scratch.empty() && !is_funct(scratch.back().funct)) {
        ++g_skips.bad_frames;
        ++g_skips.bad_at_top;
        scratch.pop_back();
    }

    // Остальной стек проверяем выборочно: он оказался когерентен всегда (ноль негодных
    // кадров в основании и середине), а сплошная проверка вымывала кэш регионов, и
    // следующий снимок делал VirtualQuery уже ВНУТРИ окна — цена росла с 0.2 % до 1.2 %.
    static thread_local std::uint64_t seen = 0;
    if ((++seen & 63) == 0) {
        for (std::size_t k = 0; k + 1 < scratch.size(); ++k) {
            ++g_skips.checked_frames;
            if (is_funct(scratch[k].funct)) { continue; }
            ++g_skips.bad_frames;
            if (k == 0) {
                ++g_skips.bad_at_bottom;
            } else {
                ++g_skips.bad_middle;
            }
        }
    }

    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    if (g_qpc_freq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        g_qpc_freq = f.QuadPart;
        g_windows.reserve(64 * 1024);
    }
    if (g_qpc_freq > 0) {
        const auto us = (t1.QuadPart - t0.QuadPart) * 1'000'000 / g_qpc_freq;
        g_windows.push_back(static_cast<std::uint32_t>(us));
    }

    out = scratch;
    return out.size();
}

bool probe(void* thread, log_fn say) {
    if (g_layout.ok()) { return true; }
    if (!graft::mt::available()) {
        say(std::format("машина недоступна: {}", graft::mt::unavailable_reason()));
        return false;
    }
    if (!probe_funct(say)) { return false; }

    const graft::mt::machine& m = graft::mt::vm();
    say(std::format("кадры +0x{:X} x0x{:X} (до {}), индекс +0x{:X}",
                    m.frames,
                    m.frame_size,
                    m.frame_count,
                    m.frame_index));

    // Стек пуст ровно тогда, когда скрипт не исполняется, — а он исполняется далеко не
    // в каждый момент. Поэтому ловим: приостанавливаем, смотрим, отпускаем, повторяем.
    // Первая попытка на пустом стеке — не отказ, а обычное дело.
    //
    // Ждём ДОЛГО и осознанно. Движок «найден» заметно раньше, чем скрипт начинает
    // исполняться: на живом сервере зонд с потолком в четыре секунды сдавался, хотя
    // скрипт шёл секундой позже. Зонд одноразовый, спешить ему некуда.
    const auto probe_until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    // Половина бюджета — на поиск кадра метода (с объектом). Дальше берём любой.
    const auto probe_deadline_for_object =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    constexpr int k_tries = 60000;
    std::vector<std::byte> frame(256);

    for (int attempt = 0; attempt < k_tries; ++attempt) {
        bool got = false;
        unsigned depth = 0;

        if (SuspendThread(static_cast<HANDLE>(thread)) != static_cast<DWORD>(-1)) {
            graft::mt::script_thread* th =
                graft::mt::context_view{graft::mt::context()}.current();
            if (th != nullptr && read_at(th, m.frame_index, depth) && depth > 0 &&
                depth < m.frame_count) {
                const auto* frames = reinterpret_cast<const std::byte*>(th) + m.frames;
                // Текущий кадр — ровно `frame_index` (кадры 1-based, см. capture).
                const std::size_t at = depth;
                std::memcpy(frame.data(),
                            frames + static_cast<std::size_t>(m.frame_size) * at,
                            m.frame_size);
                got = true;
            }
            ResumeThread(static_cast<HANDLE>(thread));
        }

        if (!got) {
            if (std::chrono::steady_clock::now() > probe_until) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Разбираем СНЯТУЮ копию, а не живую память: поток уже отпущен.
        unsigned func_at = ~0u;
        unsigned obj_at = ~0u;
        for (unsigned off = 0; off + sizeof(void*) <= m.frame_size; off += 8) {
            void* p = nullptr;
            std::memcpy(&p, frame.data() + off, sizeof(p));
            if (func_at == ~0u && is_funct(p)) {
                func_at = off;
                continue;
            }
            if (obj_at == ~0u && is_object(p)) { obj_at = off; }
        }

        if (func_at == ~0u) { continue; }

        // Кадр глобальной функции объекта НЕ несёт, и по нему смещение приёмника не
        // найти. Такой кадр как образец не годится: приняв его, мы бы навсегда остались
        // без имён классов — ровно это и случилось на одном прогоне, где зонду попался
        // `CreateCustomMission`. Поэтому ждём кадр МЕТОДА, и лишь когда время вышло,
        // соглашаемся на что есть.
        const bool patient = std::chrono::steady_clock::now() < probe_deadline_for_object;
        if (obj_at == ~0u && patient) { continue; }

        g_layout.frame_func = func_at;
        g_layout.frame_object = obj_at;
        // `m_psStat` стоит в кадре прямо перед `m_pfFunc` — подтверждено разбором
        // GetCallStack (`lea rcx,[rbx-8]` при `mov rcx,[rbx]` = Funct).
        g_layout.frame_stat = func_at >= 8 ? func_at - 8 : ~0u;

        void* funct = nullptr;
        std::memcpy(&funct, frame.data() + func_at, sizeof(funct));
        g_sample = funct;

        // Копим выборку разных функций: по ней дальше ищется `DebugOffset`. Ловим со
        // стека, а не из таблиц движка, — так в выборку попадает ровно то, что реально
        // исполняется, включая переопределения модов.
        // Выборка нужна маленькая и БЫСТРАЯ: пока она копится, съём не начался, а
        // нагрузка на сервере ждать не станет. Двадцати функций хватает на проверку
        // раскладки с запасом.
        std::vector<raw_frame> more;
        for (int n = 0; n < 150 && g_samples.size() < 24; ++n) {
            capture(thread, more);
            for (const raw_frame& rf : more) {
                if (rf.funct == nullptr || rf.object == nullptr) { continue; }
                const bool seen = std::any_of(
                    g_samples.begin(), g_samples.end(),
                    [&](const raw_frame& s2) { return s2.funct == rf.funct; });
                if (!seen) { g_samples.push_back(rf); }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        say(std::format("выборка функций для поиска отладки: {}", g_samples.size()));
        say(std::format("кадр: Funct* +0x{:X}, объект +{}, глубина {}, поймано '{}'",
                        func_at,
                        obj_at == ~0u ? std::string{"нет"} : std::format("0x{:X}", obj_at),
                        depth,
                        funct_name(funct)));
        return true;
    }

    say("за отведённые попытки скрипт так и не пошёл — кадр не опознан");
    return false;
}

std::string_view funct_name(void* funct) {
    if (funct == nullptr || g_layout.funct_name == ~0u) { return {}; }
    void* name = nullptr;
    if (!read_at(funct, g_layout.funct_name, name) || !looks_like_name(name)) { return {}; }
    return static_cast<const char*>(name);
}

std::string_view class_name(void* object) {
    if (object == nullptr) { return {}; }
    void* klass = nullptr;
    if (!read_at(object, graft::layout::object_class, klass)) { return {}; }
    void* name = nullptr;
    if (!read_at(klass, graft::layout::class_name, name) || !looks_like_name(name)) { return {}; }
    return static_cast<const char*>(name);
}

}  // namespace krprof::engine
