#pragma once
#include <string>

#include "krprof/call_tree.hpp"
#include "krprof/sample_log.hpp"
#include "krprof/sidecar.hpp"
#include "krprof/symbols.hpp"

// Отчёт страницей: сводка по модам, цепочки переопределений, самые дорогие функции.
//
// Зачем он рядом с флеймграфом. Флеймграф показывает ФОРМУ стека, а спрашивают обычно
// про цену: сколько стоит мод, сколько стоит функция, кто из пяти модов, переопределивших
// `DayZGame::OnUpdate`, съел больше. В флеймграфе это пять почти одинаковых полосок,
// которые глазом не развести; здесь — таблица.
//
// Страница самодостаточна: ни скриптов, ни шрифтов, ни запросов наружу. Отчёт открывают
// с диска сервера, где интернета может не быть вовсе.
namespace krprof {

[[nodiscard]] std::string to_html(const call_tree& tree,
                                  const sample_log& log,
                                  const symbol_table& symbols,
                                  const run_meta& meta);

}  // namespace krprof
