// Атрибуция: от кадра до исходного файла, а значит до мода.
//
// Это ЕДИНСТВЕННЫЙ достоверный источник «чей это код». Ни класс, ни скриптовый модуль
// (`3_Game`, `4_World`) мода не называют, а цепочка наследования говорит о базе, а не о
// том, кто её переопределил. В живом профиле это видно прямо: пять вложенных кадров
// `DayZGame::OnUpdate` — это пять модов, и различить их можно ТОЛЬКО по файлу.
//
// ── Раскладка СНЯТА С КОДА ИГРЫ, а не подобрана ──────────────────────────────
// Разбор целиком: ANALYSIS/02-debuginfo-подтверждено.md. Коротко: движок сам печатает
// стек вызовов с файлом и строкой, и по строкам формата (`"\n%s:%d Function %s"`)
// находится `ScriptContext::GetCallStack`, а из неё — `VMStack::GetDebugOffset` и
// `DebugInfo::GetLineInfoByOffset`. Все числа ниже прочитаны из их инструкций.
//
// Три захода поиском по памяти до этого провалились, и разбор объяснил почему:
//
//   * `Funct::DebugOffset` в рантайме НЕ используется. Движок индекс не хранит, а
//     вычисляет из `m_psStat`; мы же перебирали поля `Funct`, которого там нет;
//   * записи таблицы путей — плоские `char*` (в исходниках 0.45 это `DString`, то есть
//     указатель на заголовок; в retail — нет);
//   * самый первый зонд был ПРАВ насчёт «пути +0x0, строки +0x30», а разъехалось всё на
//     счётчике: он брался эвристикой и попадал в `size` вместо `count`.
//
// Побочная выгода нового пути: строка берётся от `m_psStat`, то есть это ТЕКУЩАЯ строка
// исполнения, а не первая строка функции.
#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include <graft/script.hpp>

#include "engine_mem.hpp"
#include "krprof/engine.hpp"

namespace krprof::engine {
namespace {

using namespace krprof::engine::mem;

// ── Раскладка ────────────────────────────────────────────────────────────────
// Записана, а не ищется, — как `graft::layout`. Патч движка её сломает, и сломает
// ГРОМКО: проверка на старте не сойдётся, и профайлер честно откажется от атрибуции.
namespace at {
inline constexpr unsigned funct_module = 0x40;  // Funct -> ScriptModule*
inline constexpr unsigned module_code = 0x78;   // ScriptModule -> буфер кода
inline constexpr unsigned code_data = 0x20;     // буфер -> указатель на данные
inline constexpr unsigned module_debug = 0x80;  // ScriptModule -> DebugInfo*

inline constexpr unsigned files = 0x00;         // DebugInfo -> const char**
inline constexpr unsigned files_count = 0x0C;
inline constexpr unsigned lines = 0x30;         // DebugInfo -> Line*
inline constexpr unsigned lines_count = 0x3C;

// Line, 8 байт: {uint m_iOffset; ushort m_iSourceLine; ushort m_iModule}
inline constexpr unsigned line_size = 8;
inline constexpr unsigned line_src = 4;
inline constexpr unsigned line_file = 6;
}  // namespace at

struct line_entry {
    std::uint32_t offset = 0;
    std::uint16_t src = 0;
    std::uint16_t file = 0;
};

bool read_line(const void* lines, unsigned index, line_entry& out) {
    const auto* at_ = static_cast<const std::byte*>(lines) +
                      static_cast<std::size_t>(index) * at::line_size;
    if (!readable(at_, at::line_size)) { return false; }
    std::memcpy(&out.offset, at_, 4);
    std::memcpy(&out.src, at_ + at::line_src, 2);
    std::memcpy(&out.file, at_ + at::line_file, 2);
    return true;
}

// Смещение текущей инструкции в коде модуля — ровно как считает движок:
//   offset = (uint32)stat - (uint32)*(void**)(*(void**)(Module + 0x78) + 0x20)
// Вычитание 32-битное и у движка тоже: обе величины лежат в одном сегменте.
bool code_offset(void* module, void* stat, std::uint32_t& out) {
    void* buffer = nullptr;
    void* data = nullptr;
    if (!read_at(module, at::module_code, buffer)) { return false; }
    if (!read_at(buffer, at::code_data, data)) { return false; }
    if (stat == nullptr || data == nullptr) { return false; }

    out = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(stat)) -
          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data));
    return true;
}

// Двоичный поиск последней строки, чьё смещение не больше заданного. Таблица
// отсортирована по смещению — это её инвариант, движок сам ищет по ней так же.
bool find_line(const void* lines, unsigned count, std::uint32_t offset, line_entry& out) {
    if (count == 0) { return false; }

    unsigned lo = 0;
    unsigned hi = count;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        line_entry e;
        if (!read_line(lines, mid, e)) { return false; }
        if (e.offset <= offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) { return false; }
    return read_line(lines, lo - 1, out);
}

}  // namespace

// Файл и строка по кадру. Именно по КАДРУ, а не по `Funct`: движок берёт позицию из
// `m_psStat`, и это даёт текущую строку исполнения.
source_ref source_of_frame(void* funct, void* stat) {
    if (funct == nullptr || stat == nullptr) { return {}; }

    void* module = nullptr;
    void* dbg = nullptr;
    if (!read_at(funct, at::funct_module, module)) { return {}; }
    if (!read_at(module, at::module_debug, dbg)) { return {}; }

    std::uint32_t offset = 0;
    if (!code_offset(module, stat, offset)) { return {}; }

    unsigned lines_count = 0;
    void* lines = nullptr;
    if (!read_at(dbg, at::lines_count, lines_count) || lines_count == 0) { return {}; }
    if (!read_at(dbg, at::lines, lines)) { return {}; }

    line_entry e;
    if (!find_line(lines, lines_count, offset, e)) { return {}; }

    unsigned files_count = 0;
    void* files = nullptr;
    if (!read_at(dbg, at::files_count, files_count) || e.file >= files_count) { return {}; }
    if (!read_at(dbg, at::files, files)) { return {}; }

    void* path = nullptr;
    if (!read_at(files, static_cast<unsigned>(e.file) * 8u, path)) { return {}; }
    // Последний рубеж: раскладка могла поехать на патче игры. Мусор в отчёт не пустим —
    // он уехал бы туда как имя мода.
    if (!looks_like_source_path(path)) { return {}; }

    return {std::string_view{static_cast<const char*>(path)}, e.src};
}

// Проверка раскладки на старте. Не поиск — именно проверка: берём кадры из живого стека
// и требуем, чтобы у большинства разрешился путь. Не сошлось — работаем без атрибуции,
// но не врём.
bool probe_debug(log_fn say) {
    layout& L = detail::mutable_layout();
    if (L.has_debug()) { return true; }

    const std::vector<raw_frame>& samples = probe_samples();
    if (samples.empty()) {
        say("выборки нет — отладку не проверить");
        return false;
    }

    std::size_t ok = 0;
    std::size_t native = 0;
    std::string_view example;
    for (const raw_frame& f : samples) {
        if (is_native(f.funct)) {
            ++native;
            continue;
        }
        const source_ref r = source_of_frame(f.funct, f.stat);
        if (!r.file.empty()) {
            ++ok;
            if (example.empty()) { example = r.file; }
        }
    }

    const std::size_t scripted = samples.size() - native;
    if (scripted == 0 || ok * 2 < scripted) {
        say(std::format("раскладка отладки не подтвердилась: разрешилось {}/{} скриптовых "
                        "(нативов {}) — атрибуции не будет",
                        ok, scripted, native));
        return false;
    }

    L.funct_module = at::funct_module;
    L.module_debug = at::module_debug;
    L.dbg_modules = at::files;
    L.dbg_module_count = at::files_count;
    L.dbg_lines = at::lines;
    L.dbg_line_count = at::lines_count;
    L.funct_debug_off = 0;  // не используется: индекс вычисляется, а не хранится

    say(std::format("отладка подтверждена: разрешилось {}/{} скриптовых, нативов {}",
                    ok, scripted, native));
    say(std::format("пример пути: {}", example));
    return true;
}

bool is_native(void* funct) {
    if (funct == nullptr) { return false; }
    std::uint32_t flags = 0;
    if (!read_at(funct, graft::layout::desc_flags, flags)) { return false; }
    return (flags & graft::layout::flag_native) != 0;
}

}  // namespace krprof::engine
