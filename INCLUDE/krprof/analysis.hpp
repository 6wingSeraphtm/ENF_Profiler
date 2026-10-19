#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "krprof/call_tree.hpp"
#include "krprof/sample_log.hpp"
#include "krprof/symbols.hpp"

// Свёртки для отчёта.
//
// Флеймграф отвечает на вопрос «какой формы был стек». Спрашивают обычно другое: сколько
// стоит функция и кто из модов сколько отъел у общего метода. В флеймграфе пять
// вложенных `DayZGame::OnUpdate` — пять почти одинаковых полосок, и глазом их не
// развести. Здесь то же самое становится таблицей, а таблица читается.
namespace krprof {

struct func_stat {
    frame_id id = 0;
    std::uint64_t self = 0;
};

// Собственный вес по кадрам, по убыванию. Кадры без собственного веса не попадают:
// они только звали других, и в таблице «что дорого» им делать нечего.
[[nodiscard]] std::vector<func_stat> by_function(const call_tree& tree,
                                                 const symbol_table& symbols);

struct override_link {
    std::string mod;
    std::uint64_t self = 0;
    std::string file;
    std::uint32_t line = 0;
};

// Один метод, переопределённый несколькими модами.
struct override_chain {
    std::string name;  // `Класс::Метод`
    std::uint64_t total = 0;
    std::vector<override_link> links;  // по убыванию цены
};

// Методы, у которых больше одного мода в цепочке. Это и есть ответ на вопрос «во что
// обходится конкретный override конкретным модом» — тот самый, который ванильный
// EnProfiler отбрасывает намеренно (`FromSuper`).
[[nodiscard]] std::vector<override_chain> override_chains(const call_tree& tree,
                                                          const symbol_table& symbols);

// Узел флеймграфа. В отличие от свёрнутых стеков здесь есть СУММАРНЫЙ вес: ширина
// полоски — это вес всего поддерева, и считать его в браузере на каждом кадре незачем.
struct flame_node {
    frame_id id = 0;
    std::uint64_t total = 0;  // собственный вес плюс всё поддерево
    std::uint64_t self = 0;
    std::uint32_t depth = 0;
    bool is_root = false;  // синтетическое основание: у флеймграфа одна опора
    std::vector<std::uint32_t> children;
};

// Плоский массив узлов; [0] — основание. Дети упорядочены по убыванию веса: широкое
// слева читается первым, и картинка не зависит от обхода хеш-таблицы.
[[nodiscard]] std::vector<flame_node> flame_tree(const call_tree& tree);

}  // namespace krprof

namespace krprof {

// Один непрерывный эпизод исполнения скрипта — практически один игровой кадр.
struct episode {
    std::uint64_t start_us = 0;
    std::uint64_t dur_us = 0;
    std::uint64_t samples = 0;
    frame_id top = 0;             // самый частый лист внутри эпизода
    std::uint64_t top_samples = 0;
};

// Разбить прогон на эпизоды и отсортировать по убыванию длительности.
//
// Это объективный ответ на «куда ушло время в конкретном кадре»: сводка по модам
// говорит про весь прогон, флеймграф — про форму стека, а «сервер дёрнулся, что это
// было» видно только здесь. Границей служит разрыв больше шага съёма: подряд идущие
// снимки означают, что скрипт работал не останавливаясь.
[[nodiscard]] std::vector<episode> episodes(const sample_log& log,
                                            const symbol_table& symbols,
                                            std::uint64_t period_us);

}  // namespace krprof
