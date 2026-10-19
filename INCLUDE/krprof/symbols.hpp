#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "krprof/call_tree.hpp"

// Кто скрывается за кадром: мод, класс, функция, файл.
//
// Разбор одного кадра стоит дорого — обход отладочной информации движка, разбор пути,
// поиск объявляющего класса. Снимков миллионы, а РАЗНЫХ кадров тысячи, поэтому разбор
// делается один раз на кадр и запоминается. Ключ даёт движковая половина: для метода это
// пара (Funct*, Class*), для глобальной функции — сам Funct*.
namespace krprof {

// Три мира, которые надо различать, а не сваливать в одну кучу.
enum class frame_kind : std::uint8_t {
    script,   // байткод: есть отладочная информация, значит есть мод и файл
    proto,    // натив движка, вызванный из скрипта: имя скриптовое, исходника нет
    engine,   // C++ движка из стека ОС: имени нет, есть модуль и смещение
    unknown,  // разобрать не удалось — так и скажем
};

struct symbol {
    frame_kind kind = frame_kind::unknown;
    // Из отладочной информации: `Funct -> DebugOffset -> DebugInfo -> путь -> мод`.
    // Это ЕДИНСТВЕННЫЙ достоверный источник принадлежности — см. attribution.hpp.
    std::string mod;
    // Класс, которому принадлежит исполняемый `Funct`. Пусто у глобальной функции.
    std::string klass;
    std::string func;
    // Файл и строка объявления — для сайдкара: по ним прыгают в код.
    std::string file;
    std::uint32_t line = 0;
};

class symbol_table {
public:
    // Вернуть id кадра по ключу. Если ключ новый — позвать `make` и запомнить результат.
    // `make` зовётся РОВНО один раз на ключ: в ней и живёт вся дорогая работа.
    template <class Fn>
    frame_id intern(std::uint64_t key, Fn&& make) {
        const auto it = by_key_.find(key);
        if (it != by_key_.end()) { return it->second; }

        const auto id = static_cast<frame_id>(symbols_.size());
        symbols_.push_back(std::forward<Fn>(make)());
        by_key_.emplace(key, id);
        return id;
    }

    // Символ по id. Неизвестный id — не ошибка: id уезжает в дерево и живёт там своей
    // жизнью, а падать профайлеру нельзя. Отдаём пустой символ, эмиттер назовёт его сам.
    [[nodiscard]] const symbol& operator[](frame_id id) const noexcept {
        return id < symbols_.size() ? symbols_[id] : none_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return symbols_.size(); }

    // Пройти по всем символам и при желании поправить их. Нужно ровно одному: пересчёту
    // имён модов на выгрузке, когда набор путей уже собран целиком (см. mod_map).
    template <class Fn>
    void for_each(Fn&& fn) {
        for (symbol& s : symbols_) { fn(s); }
    }

    void clear() noexcept {
        symbols_.clear();
        by_key_.clear();
    }

private:
    std::vector<symbol> symbols_;
    std::unordered_map<std::uint64_t, frame_id> by_key_;

    inline static const symbol none_{};
};

}  // namespace krprof
