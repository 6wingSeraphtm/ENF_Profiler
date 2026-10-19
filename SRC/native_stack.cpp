#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "engine_mem.hpp"
#include "krprof/native_stack.hpp"

// ── Почему схема именно такая ────────────────────────────────────────────────
//
// Первая версия делала «как в учебнике»: `RtlVirtualUnwind` по контексту потока. Она
// УРОНИЛА СЕРВЕР — `Fault address: Unknown module`, минидамп, конец прогона. Две ошибки
// сразу, и обе поучительные:
//
//   1. Раскрутка шла ПОСЛЕ `ResumeThread`. Поток уже бежал и переписывал собственный
//      стек под нами, поэтому в контексте оказывался мусор, а `RtlVirtualUnwind` с
//      мусором разыменовывает что попало.
//   2. Не было защиты от сбоя. Исключение в потоке профайлера — это падение ВСЕГО
//      процесса, то есть сервера. Профайлер, роняющий сервер, хуже отсутствующего.
//
// Есть и третья опасность, которая не выстрелила, но выстрелила бы:
// `RtlLookupFunctionEntry` берёт блокировку таблицы функций загрузчика. Если
// приостановленный поток держит её же — взаимоблокировка намертво.
//
// Отсюда правило: ПОКА ПОТОК СТОИТ, делаем только копирование. Ни разбора, ни поиска,
// ни блокировок. Всё остальное — на копии, после возобновления, и под защитой.
namespace krprof::native {
namespace {

using namespace krprof::engine::mem;

// ── Карта имён из исходников ─────────────────────────────────────────────────
// Файл `krprof.symbols` рядом с exe: строки `RVA<TAB>имя`. Его печатает
// ANALYSIS/research/mapgen.py, сопоставляя строковые литералы исходников Enfusion 0.45
// с образом игры. PDB у игры нет, экспортов три — это единственный способ получить
// настоящие имена движковых функций.
//
// Карта заведомо НЕПОЛНАЯ (исходники старые и покрывают малую часть образа), поэтому
// она дополняет `модуль+RVA`, а не заменяет: не нашлось имени — печатаем адрес.
std::unordered_map<std::uintptr_t, std::string> g_map;
std::once_flag g_map_once;

void load_map() {
    std::call_once(g_map_once, [] {
        std::ifstream in{"krprof.symbols"};
        if (!in) { return; }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') { continue; }
            const std::size_t tab = line.find('	');
            if (tab == std::string::npos) { continue; }
            const auto rva = std::strtoull(line.c_str(), nullptr, 0);
            if (rva != 0) { g_map.emplace(rva, line.substr(tab + 1)); }
        }
    });
}

std::once_flag g_sym_once;
std::atomic<bool> g_sym_ready{false};

void init_symbols() {
    std::call_once(g_sym_once, [] {
        // SYMOPT_UNDNAME — тот самый demangle. На самой игре толку немного (PDB нет, три
        // экспорта), но системные библиотеки имена отдают, и там он отрабатывает.
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        g_sym_ready.store(SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE);
    });
}

// Похож ли адрес на точку возврата: у него есть запись в таблице раскрутки. Проверка
// делается СНАРУЖИ окна, когда ничего не приостановлено, — блокировка загрузчика в этот
// момент безопасна.
bool looks_like_return(std::uintptr_t v) {
    if (v < 0x10000) { return false; }
    DWORD64 image = 0;
    return RtlLookupFunctionEntry(v, &image, nullptr) != nullptr;
}

}  // namespace

std::size_t capture_stack(void* thread, snapshot& out) {
    out.rip = 0;
    out.rsp = 0;
    out.bytes.clear();

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(static_cast<HANDLE>(thread), &ctx) == FALSE) { return 0; }

    out.rip = ctx.Rip;
    out.rsp = ctx.Rsp;

    // Копируем кусок стека и уходим. Только memcpy — ни поиска, ни блокировок.
    //
    // ПОШАГОВО, а не одним куском: сплошная проверка «читаемы ли сразу 8 КБ» почти
    // всегда проваливалась — окно упиралось в границу региона, и снимок отбрасывался
    // целиком. За прогон так уцелело два кадра из тысяч попыток.
    constexpr std::size_t k_page = 4096;
    constexpr std::size_t k_max = 16 * 1024;
    out.bytes.reserve(k_max);
    for (std::size_t at = 0; at < k_max; at += k_page) {
        const auto* from = reinterpret_cast<const std::byte*>(ctx.Rsp) + at;
        if (!readable(from, k_page)) { break; }
        out.bytes.insert(out.bytes.end(), from, from + k_page);
    }
    return out.bytes.size();
}

std::size_t walk(const snapshot& snap, std::vector<void*>& out, std::size_t limit) {
    out.clear();
    if (snap.rip == 0) { return 0; }

    // Верхний кадр — там, где поток стоял на самом деле.
    if (looks_like_return(snap.rip)) { out.push_back(reinterpret_cast<void*>(snap.rip)); }

    // Дальше — КОНСЕРВАТИВНЫЙ проход по копии стека: берём всё, что похоже на адрес
    // возврата. Это не настоящая раскрутка, и честно называть это надо так: среди
    // найденного будут остатки прошлых вызовов, то есть кадры, которых уже нет.
    //
    // Почему всё равно так, а не `RtlVirtualUnwind`: та требует ЖИВОГО стека, то есть
    // работы под приостановкой, а под приостановкой нельзя ни искать записи раскрутки
    // (блокировка загрузчика), ни падать. Приблизительный, но безопасный ответ лучше
    // точного, который раз в час роняет сервер.
    const auto* words = reinterpret_cast<const std::uintptr_t*>(snap.bytes.data());
    const std::size_t count = snap.bytes.size() / sizeof(std::uintptr_t);

    std::uintptr_t prev = 0;
    for (std::size_t i = 0; i < count && out.size() < limit; ++i) {
        const std::uintptr_t v = words[i];
        if (v == prev) { continue; }  // одна и та же ссылка подряд — не два кадра
        if (!looks_like_return(v)) { continue; }
        prev = v;
        out.push_back(reinterpret_cast<void*>(v));
    }
    return out.size();
}

void* function_start(void* addr) {
    DWORD64 image = 0;
    RUNTIME_FUNCTION* fn =
        RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(addr), &image, nullptr);
    if (fn == nullptr) { return addr; }
    return reinterpret_cast<void*>(image + fn->BeginAddress);
}

std::string name_of(void* addr) {
    init_symbols();
    load_map();

    HMODULE mod = nullptr;
    char path[MAX_PATH]{};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod) != FALSE &&
        GetModuleFileNameA(mod, path, MAX_PATH) != 0) {
        const char* slash = std::strrchr(path, '\\');
        const std::string file{slash != nullptr ? slash + 1 : path};

        // Имя символа — если оно есть. У игры его не будет (PDB нет, три экспорта),
        // у системных библиотек будет и уже с demangle.
        if (g_sym_ready.load()) {
            alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + 512> buf{};
            auto* info = reinterpret_cast<SYMBOL_INFO*>(buf.data());
            info->SizeOfStruct = sizeof(SYMBOL_INFO);
            info->MaxNameLen = 500;
            DWORD64 disp = 0;
            // Смещение обязано быть МАЛЕНЬКИМ. dbghelp отдаёт БЛИЖАЙШИЙ символ, а у
            // самой игры их три на весь образ — поэтому без этой проверки любой адрес
            // получал имя `CDPCreateServer` и отстоял от него на мегабайты. Это было бы
            // не имя, а враньё: читатель поверил бы, что знает функцию.
            constexpr DWORD64 k_max_disp = 4096;
            if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &disp,
                            info) != FALSE &&
                info->NameLen != 0 && disp < k_max_disp) {
                std::string out = file + "!" + std::string{info->Name, info->NameLen};
                if (disp != 0) {
                    std::array<char, 24> off{};
                    std::snprintf(off.data(), off.size(), "+0x%llx",
                                  static_cast<unsigned long long>(disp));
                    out.append(off.data());
                }
                return out;
            }
        }

        const auto rva = reinterpret_cast<std::uintptr_t>(addr) -
                         reinterpret_cast<std::uintptr_t>(mod);

        // Карта из исходников. Ищем точное начало функции: сюда приходит уже
        // `function_start`, поэтому подгонять смещение не нужно.
        if (const auto it = g_map.find(rva); it != g_map.end()) {
            return file + "!" + it->second;
        }

        // Запасной путь, который есть ВСЕГДА: модуль плюс смещение. По нему функция
        // ищется в дизассемблере, и между прогонами оно не плывёт.
        std::array<char, 32> off{};
        std::snprintf(off.data(), off.size(), "+0x%llx", static_cast<unsigned long long>(rva));
        return file + off.data();
    }

    std::array<char, 32> raw{};
    std::snprintf(raw.data(), raw.size(), "0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr)));
    return raw.data();
}

}  // namespace krprof::native
