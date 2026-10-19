#pragma once
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

// Дерево стеков — то, чего нет у ванильного профайлера и без чего флеймграф не строится.
//
// EnProfiler держит счётчики на самих `Funct` и `Class`: одно число на функцию, без
// оглядки на то, кто её позвал. Из такого дерево не восстановить — оно уже сложено.
// Здесь наоборот: единица учёта не функция, а ПУТЬ до неё, поэтому «PlayerBase::OnUpdate
// из тика миссии» и «он же из UI» остаются разными строками, а рекурсия не двоится в
// один счётчик.
//
// Кадр здесь — просто число. Ни функций, ни модов, ни движка дерево не знает: перевод
// `frame_id` в «мод, класс, функция» живёт в таблице символов и делается ОДИН РАЗ на
// кадр, а не на сэмпл.
namespace krprof {

// Индекс в таблице символов. Своей идентичности у кадра нет — только эта ссылка.
using frame_id = std::uint32_t;

// Один лист развёртки: путь снаружи внутрь и СОБСТВЕННЫЙ вес этого пути.
//
// Именно self, а не суммарный: в формате свёрнутых стеков каждая строка несёт свой вес,
// а вес предков просмотрщик складывает сам. Отдать здесь сумму значит посчитать всё
// дважды.
struct folded_entry {
    std::vector<frame_id> path;
    std::uint64_t self = 0;

    bool operator==(const folded_entry&) const = default;
};

class call_tree {
public:
    // Положить снятый стек. `stack[0]` — самый внешний кадр: массив кадров скриптового
    // потока лежит именно так, и разворачивать его незачем.
    //
    // Вес — это то, что мы измеряем. У сэмплера единица (один снимок), но параметр не
    // ради общности: при неравномерном шаге таймера честный вес — прошедшее время, а не
    // счёт снимков.
    void add(std::span<const frame_id> stack, std::uint64_t weight = 1);

    void clear() noexcept;

    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    [[nodiscard]] std::size_t nodes() const noexcept { return nodes_.size(); }

    // Развернуть в строки для эмиттера. Порядок детерминированный (лексикографический по
    // пути): отчёты сравнивают между прогонами, и переставшие совпадать строки не должны
    // быть просто другим порядком обхода хеш-таблицы.
    //
    // Аллоцирует и сортирует — и это нормально: зовётся на выгрузке, а не на снимке.
    [[nodiscard]] std::vector<folded_entry> flatten() const;

private:
    static constexpr std::uint32_t no_parent = 0xFFFF'FFFFu;

    struct node {
        frame_id id = 0;
        std::uint32_t parent = no_parent;
        std::uint64_t self = 0;
    };

    // Ребро (родитель, кадр) -> узел. Одна хеш-таблица на всё дерево, а не по таблице на
    // узел: узлов десятки тысяч, и своя таблица у каждого — это десятки тысяч аллокаций
    // на ровном месте.
    static constexpr std::uint64_t edge_key(std::uint32_t parent, frame_id id) noexcept {
        return (static_cast<std::uint64_t>(parent) << 32) | id;
    }

    std::vector<node> nodes_;
    std::unordered_map<std::uint64_t, std::uint32_t> edges_;
    std::uint64_t total_ = 0;
};

}  // namespace krprof
