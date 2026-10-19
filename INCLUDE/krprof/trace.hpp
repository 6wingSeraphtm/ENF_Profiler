#pragma once
#include <string>

#include "krprof/sample_log.hpp"
#include "krprof/sidecar.hpp"
#include "krprof/symbols.hpp"

// Chrome Trace Event Format — вход для готовых, популярных просмотрщиков:
//
//   Perfetto          ui.perfetto.dev          трассы, зум, запросы
//   Firefox Profiler  profiler.firefox.com     флеймграф, дерево вызовов, инверсия
//   Chrome DevTools   вкладка Performance
//
// Один формат — три приложения, и ни одно не надо ставить. Поэтому он и выбран: свой
// просмотрщик писать незачем, а этот формат читают все.
//
// Сэмплер даёт СНИМКИ, формат хочет ОТРЕЗКИ. Превращение одно: пока у соседних снимков
// общий префикс стека, кадр держится открытым; разошлись — закрываем кончившееся и
// открываем начавшееся. Провал между снимками рвёт отрезок: скрипт в это время не
// исполнялся, и тянуть полосу через простой значило бы соврать.
namespace krprof {

[[nodiscard]] std::string to_chrome_trace(const sample_log& log,
                                          const symbol_table& symbols,
                                          const run_meta& meta);

}  // namespace krprof
