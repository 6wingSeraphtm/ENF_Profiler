// speedscope `sampled` — основной формат отчёта.
//
// Свёрнутые стеки агрегированы: времени в них нет, и покадрово по ним не посмотреть.
// А игра однопоточна и идёт кадрами, поэтому вопрос «какой кадр был на 200 мс и что в
// нём было» важнее суммарной таблицы. Этот формат хранит снимки В ПОРЯДКЕ ВРЕМЕНИ,
// поэтому даёт и флеймграф, и режим Time Order — то есть покадровый разбор.
#include <gtest/gtest.h>

#include "krprof/sample_log.hpp"
#include "krprof/speedscope.hpp"
#include "krprof/symbols.hpp"

using krprof::frame_id;
using krprof::run_meta;
using krprof::sample_log;
using krprof::symbol;
using krprof::symbol_table;
using krprof::to_speedscope_json;

namespace {

void put(sample_log& log, std::initializer_list<frame_id> stack, std::uint64_t at_us) {
    log.add(std::span<const frame_id>{stack.begin(), stack.size()}, at_us);
}

frame_id sym(symbol_table& t, std::string mod, std::string klass, std::string func) {
    return t.intern(t.size() + 1, [&] {
        symbol s;
        s.mod = std::move(mod);
        s.klass = std::move(klass);
        s.func = std::move(func);
        s.file = "TraderPlus/scripts/4_World/x.c";
        s.line = 12;
        return s;
    });
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── Журнал снимков ───────────────────────────────────────────────────────────
TEST(SampleLog, EmptyLogIsEmpty) {
    const sample_log log;
    EXPECT_EQ(log.size(), 0u);
}

TEST(SampleLog, KeepsStacksInOrder) {
    sample_log log;
    put(log, {1, 2}, 1000);
    put(log, {3}, 2000);

    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log.stack_at(0).size(), 2u);
    EXPECT_EQ(log.stack_at(0)[0], 1u);
    EXPECT_EQ(log.stack_at(0)[1], 2u);
    EXPECT_EQ(log.stack_at(1).size(), 1u);
    EXPECT_EQ(log.stack_at(1)[0], 3u);
    EXPECT_EQ(log.time_at(0), 1000u);
    EXPECT_EQ(log.time_at(1), 2000u);
}

// Порядок — это весь смысл журнала: одинаковые стеки НЕ схлопываются, иначе таймлайна
// не будет. Схлопывание живёт в дереве, и это другая структура.
TEST(SampleLog, IdenticalStacksAreKeptSeparately) {
    sample_log log;
    put(log, {1}, 100);
    put(log, {1}, 200);
    EXPECT_EQ(log.size(), 2u);
}

TEST(SampleLog, ClearResets) {
    sample_log log;
    put(log, {1}, 100);
    log.clear();
    EXPECT_EQ(log.size(), 0u);
}

// ── Эмиттер ──────────────────────────────────────────────────────────────────
TEST(Speedscope, EmptyProfileIsValidJson) {
    const auto json = to_speedscope_json(sample_log{}, symbol_table{}, run_meta{});
    EXPECT_TRUE(has(json, "\"$schema\""));
    EXPECT_TRUE(has(json, "\"type\": \"sampled\""));
    EXPECT_TRUE(has(json, "\"samples\": []"));
    EXPECT_TRUE(json.ends_with("}\n"));
}

TEST(Speedscope, FramesGoIntoSharedTable) {
    symbol_table syms;
    const auto a = sym(syms, "TraderPlus", "PlayerBase", "OnUpdate");
    const auto b = sym(syms, "$vanilla", "PlayerBase", "OnUpdate");

    sample_log log;
    put(log, {a, b}, 0);

    const auto json = to_speedscope_json(log, syms, run_meta{});
    EXPECT_TRUE(has(json, "\"name\": \"TraderPlus|PlayerBase::OnUpdate\""));
    EXPECT_TRUE(has(json, "\"name\": \"$vanilla|PlayerBase::OnUpdate\""));
    EXPECT_TRUE(has(json, "\"samples\": [[0,1]]"));
}

// Файл и строка едут в таблице кадров — по ним прыгают в код прямо из просмотрщика.
TEST(Speedscope, FileAndLineAreCarried) {
    symbol_table syms;
    const auto a = sym(syms, "TraderPlus", "C", "f");

    sample_log log;
    put(log, {a}, 0);

    const auto json = to_speedscope_json(log, syms, run_meta{});
    EXPECT_TRUE(has(json, "\"file\": \"TraderPlus/scripts/4_World/x.c\""));
    EXPECT_TRUE(has(json, "\"line\": 12"));
}

// Один и тот же кадр в разных снимках — ОДНА запись в таблице и один индекс.
TEST(Speedscope, RepeatedFrameIsSharedNotDuplicated) {
    symbol_table syms;
    const auto a = sym(syms, "M", "C", "f");

    sample_log log;
    put(log, {a}, 0);
    put(log, {a}, 1000);

    const auto json = to_speedscope_json(log, syms, run_meta{});
    EXPECT_EQ(json.find("M|C::f"), json.rfind("M|C::f"));
    EXPECT_TRUE(has(json, "\"samples\": [[0],[0]]"));
}

// ── Веса: настоящее время, а не счёт снимков ─────────────────────────────────
// Вес снимка — интервал до следующего. С ним ось времени настоящая, и всплеск в 200 мс
// выглядит на 200 мс, а не «на сто снимков».
TEST(Speedscope, WeightIsIntervalToNextSample) {
    symbol_table syms;
    const auto a = sym(syms, "M", "C", "f");

    sample_log log;
    put(log, {a}, 1000);
    put(log, {a}, 3000);  // 2000 мкс спустя
    put(log, {a}, 3500);  // 500 мкс спустя

    const auto json = to_speedscope_json(log, syms, run_meta{2, 500, 0, 0});
    EXPECT_TRUE(has(json, "\"weights\": [2000,500,2000]"));
}

// Провал в снимках означает, что скрипт не исполнялся вовсе, — и приписывать эту паузу
// последнему кадру нельзя, иначе один снимок перед простоем съест весь флеймграф.
// Поэтому интервал ограничен сверху периодом сэмплирования.
TEST(Speedscope, LongGapIsCappedNotAttributed) {
    symbol_table syms;
    const auto a = sym(syms, "M", "C", "f");

    sample_log log;
    put(log, {a}, 0);
    put(log, {a}, 5'000'000);  // пять секунд простоя

    // 500 Гц -> период 2000 мкс, потолок 4 периода = 8000.
    const auto json = to_speedscope_json(log, syms, run_meta{2, 500, 0, 0});
    EXPECT_TRUE(has(json, "\"weights\": [8000,2000]"));
}

TEST(Speedscope, EndValueIsSumOfWeights) {
    symbol_table syms;
    const auto a = sym(syms, "M", "C", "f");

    sample_log log;
    put(log, {a}, 0);
    put(log, {a}, 1000);

    const auto json = to_speedscope_json(log, syms, run_meta{2, 500, 0, 0});
    EXPECT_TRUE(has(json, "\"unit\": \"microseconds\""));
    EXPECT_TRUE(has(json, "\"endValue\": 3000"));
}

// Кавычки в именах ломают JSON молча — отчёт просто перестаёт открываться.
TEST(Speedscope, NamesAreEscaped) {
    symbol_table syms;
    const auto a = sym(syms, "we\"ird", "C", "f");

    sample_log log;
    put(log, {a}, 0);

    const auto json = to_speedscope_json(log, syms, run_meta{});
    EXPECT_TRUE(has(json, "we\\\"ird"));
}
