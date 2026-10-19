#pragma once
#include <string>

#include "krprof/sample_log.hpp"
#include "krprof/sidecar.hpp"
#include "krprof/symbols.hpp"

// Отчёт в формате speedscope (профиль типа `sampled`) — основной вывод профайлера.
//
// Почему он, а не только свёрнутые стеки: тот формат агрегирован, времени в нём нет, и
// покадрово по нему не посмотреть. Здесь снимки лежат в порядке времени, поэтому
// просмотрщик даёт и флеймграф (Left Heavy), и режим Time Order — то есть разбор по
// кадрам, ради которого всё и затевалось. Плюс таблица кадров несёт файл и строку, так
// что из просмотрщика можно прыгнуть в код.
namespace krprof {

[[nodiscard]] std::string to_speedscope_json(const sample_log& log,
                                             const symbol_table& symbols,
                                             const run_meta& meta);

}  // namespace krprof
