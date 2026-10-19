#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Внутреннее: безопасное чтение чужой памяти.
//
// Зонд ходит по указателям, которые ещё не проверены, — это его работа. Свалиться при
// этом он не имеет права: падение профайлера означает падение сервера, а включают
// профайлер ровно там, где и так плохо.
namespace krprof::engine::mem {

// VirtualQuery стоит около микросекунды, а поиск цепочки отладочной информации делает
// сотни тысяч проверок. Регионы при этом почти не меняются, поэтому последние ответы
// держим при себе — иначе поиск идёт десятки секунд вместо долей.
struct region {
    std::uintptr_t from = 0;
    std::uintptr_t to = 0;
    bool ok = false;
};

// Кэш маленьким быть не должен. Проверка кадров трогает два региона на кадр (сам
// `Funct` и строка имени), плюс объект потока, кадры и контекст. На шестнадцати записях
// это вымывало кэш, и следующий снимок делал VirtualQuery уже ВНУТРИ окна остановки —
// цена профайлера росла втрое (0.22 % -> 0.69 %). Регионов в игре немного, держать их
// все дёшево.
inline constexpr int k_cache = 64;
inline thread_local region t_cache[k_cache]{};
inline thread_local int t_next = 0;

inline bool readable(const void* p, std::size_t bytes) {
    if (p == nullptr) { return false; }
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    const auto want = at + bytes;

    for (const region& r : t_cache) {
        if (r.to != 0 && at >= r.from && want <= r.to) { return r.ok; }
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) { return false; }

    const auto start = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const bool ok =
        mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;

    t_cache[t_next] = region{start, start + mbi.RegionSize, ok};
    t_next = (t_next + 1) % k_cache;

    return ok && want <= start + mbi.RegionSize;
}

template <class T>
inline bool read_at(const void* base, unsigned offset, T& out) {
    const auto* at = static_cast<const std::byte*>(base) + offset;
    if (!readable(at, sizeof(T))) { return false; }
    std::memcpy(&out, at, sizeof(T));
    return true;
}

// Похоже ли на имя: печатный ASCII разумной длины с нулём на конце. Мусорный указатель
// может смотреть на мегабайт без нуля — поэтому предел обязателен.
inline bool looks_like_name(const void* p) {
    if (!readable(p, 2)) { return false; }
    const auto* s = static_cast<const char*>(p);
    for (int i = 0; i < 96; ++i) {
        if (!readable(s + i, 1)) { return false; }
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0) { return i > 0; }
        if (c < 0x20 || c > 0x7E) { return false; }
    }
    return false;
}

inline bool string_is(const void* p, std::string_view want) {
    if (!readable(p, want.size() + 1)) { return false; }
    const auto* s = static_cast<const char*>(p);
    return std::memcmp(s, want.data(), want.size()) == 0 && s[want.size()] == '\0';
}

// Путь к исходнику скрипта: содержит разделитель и кончается на `.c`. Признак сильный —
// случайный указатель такую строку не даст, и именно на нём держится весь поиск
// отладочной информации.
inline bool looks_like_source_path(const void* p) {
    if (!readable(p, 4)) { return false; }
    const auto* s = static_cast<const char*>(p);
    bool has_sep = false;
    for (int i = 0; i < 260; ++i) {
        if (!readable(s + i, 1)) { return false; }
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0) {
            return i > 2 && has_sep && s[i - 2] == '.' && (s[i - 1] | 32) == 'c';
        }
        if (c == '/' || c == '\\') { has_sep = true; }
        if (c < 0x20 || c > 0x7E) { return false; }
    }
    return false;
}

}  // namespace krprof::engine::mem
