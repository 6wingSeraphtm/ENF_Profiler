// Chrome CPU Profile внутри трассы — вход для готовых просмотрщиков:
// Firefox Profiler (profiler.firefox.com), Chrome DevTools, Perfetto, speedscope.
//
// Форму выбрали не сразу. Сначала писали парные отрезки B/E: Perfetto их принял, а
// Firefox Profiler отказал — «A tracing event in the chrome profile did not follow the
// expected form». Его импортёр ждёт от сэмплирующего профайлера ИМЕННО CPU-профиль:
// дерево узлов плюс номер узла на каждый снимок. Так и правильнее по сути — у нас
// снимки, а отрезки были домыслом.
#include <gtest/gtest.h>

#include <string>

#include "krprof/sample_log.hpp"
#include "krprof/symbols.hpp"
#include "krprof/trace.hpp"

using krprof::frame_id;
using krprof::run_meta;
using krprof::sample_log;
using krprof::symbol;
using krprof::symbol_table;
using krprof::to_chrome_trace;

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
        s.file = "KR/core/KR_CORE/JSON/1_Core/json_ser.c";
        s.line = 559;
        return s;
    });
}

bool has(const std::string& h, std::string_view n) { return h.find(n) != std::string::npos; }

}  // namespace

TEST(Trace, EmptyRunIsStillValidTrace) {
    const auto t = to_chrome_trace(sample_log{}, symbol_table{}, run_meta{});
    EXPECT_TRUE(t.starts_with("{"));
    EXPECT_TRUE(has(t, "\"traceEvents\""));
    EXPECT_TRUE(t.ends_with("}\n"));
}

// Импортёр Firefox Profiler ищет ровно эти два события; без них файл не профиль.
TEST(Trace, HasProfileAndChunkEvents) {
    const auto t = to_chrome_trace(sample_log{}, symbol_table{}, run_meta{});
    EXPECT_TRUE(has(t, "\"name\":\"Profile\""));
    EXPECT_TRUE(has(t, "\"name\":\"ProfileChunk\""));
    EXPECT_TRUE(has(t, "cpuProfile"));
}

// Каждое событие обязано нести полный набор полей — на неполном импортёр и споткнулся.
TEST(Trace, EveryEventCarriesRequiredFields) {
    const auto t = to_chrome_trace(sample_log{}, symbol_table{}, run_meta{});
    // Сколько событий, столько и «cat», «ph», «pid», «tid», «ts».
    const auto count = [&](std::string_view n) {
        std::size_t c = 0;
        for (std::size_t at = t.find(n); at != std::string::npos; at = t.find(n, at + 1)) {
            ++c;
        }
        return c;
    };
    EXPECT_EQ(count("\"ph\":"), 4u);   // process_name, thread_name, Profile, ProfileChunk
    EXPECT_EQ(count("\"cat\":"), 4u);
    EXPECT_EQ(count("\"pid\":1"), 4u);
    EXPECT_EQ(count("\"ts\":"), 4u);
}

TEST(Trace, NamesTheProcessAndThread) {
    const auto t = to_chrome_trace(sample_log{}, symbol_table{}, run_meta{});
    EXPECT_TRUE(has(t, "process_name"));
    EXPECT_TRUE(has(t, "thread_name"));
}

// Корень нумеруется единицей: Chrome считает узлы с неё.
TEST(Trace, RootNodeIsOne) {
    const auto t = to_chrome_trace(sample_log{}, symbol_table{}, run_meta{});
    EXPECT_TRUE(has(t, "\"id\":1"));
    EXPECT_TRUE(has(t, "(root)"));
}

TEST(Trace, StackBecomesNodeChain) {
    symbol_table syms;
    const auto outer = sym(syms, "KR_CORE", "JsonBuilder", "AddString");
    const auto inner = sym(syms, "KR_CORE", "JsonBuilder", "EscapeString");

    sample_log log;
    put(log, {outer, inner}, 1000);

    const auto t = to_chrome_trace(log, syms, run_meta{1, 1000, 0, 0});
    EXPECT_TRUE(has(t, "JsonBuilder::AddString"));
    EXPECT_TRUE(has(t, "JsonBuilder::EscapeString"));
    // Внутренний узел ссылается на внешний как на родителя.
    EXPECT_TRUE(has(t, "\"parent\":2"));
    // Снимок указывает на самый внутренний узел.
    EXPECT_TRUE(has(t, "\"samples\":[3]"));
}

// Одинаковые стеки делят узлы — иначе дерево вызовов рассыпалось бы на копии.
TEST(Trace, IdenticalStacksShareNodes) {
    symbol_table syms;
    const auto f = sym(syms, "M", "C", "f");

    sample_log log;
    put(log, {f}, 0);
    put(log, {f}, 1000);

    const auto t = to_chrome_trace(log, syms, run_meta{2, 1000, 0, 0});
    EXPECT_TRUE(has(t, "\"samples\":[2,2]"));
    EXPECT_TRUE(has(t, "\"timeDeltas\":[0,1000]"));
}

// Первый промежуток нулевой: профиль начинается ровно с первого снимка.
TEST(Trace, FirstDeltaIsZero) {
    symbol_table syms;
    const auto f = sym(syms, "M", "C", "f");

    sample_log log;
    put(log, {f}, 5000);
    put(log, {f}, 5250);

    const auto t = to_chrome_trace(log, syms, run_meta{2, 1000, 0, 0});
    EXPECT_TRUE(has(t, "\"timeDeltas\":[0,250]"));
    EXPECT_TRUE(has(t, "\"startTime\":5000"));
    EXPECT_TRUE(has(t, "\"endTime\":5250"));
}

// Путь исходника уезжает в `url`: по нему просмотрщик показывает файл, а начинается он
// с префикса мода — значит мод виден и отдельного поля не просит.
TEST(Trace, SourceGoesToUrl) {
    symbol_table syms;
    const auto f = sym(syms, "KR_CORE", "JsonBuilder", "AddString");

    sample_log log;
    put(log, {f}, 0);

    const auto t = to_chrome_trace(log, syms, run_meta{1, 1000, 0, 0});
    EXPECT_TRUE(has(t, "\"url\":\"KR/core/KR_CORE/JSON/1_Core/json_ser.c\""));
    // Chrome считает строки с нуля, движок — с единицы.
    EXPECT_TRUE(has(t, "\"lineNumber\":558"));
}

TEST(Trace, GlobalFunctionHasNoClassPrefix) {
    symbol_table syms;
    const auto f = sym(syms, "M", "", "GetTraderConfig");

    sample_log log;
    put(log, {f}, 0);

    const auto t = to_chrome_trace(log, syms, run_meta{1, 1000, 0, 0});
    EXPECT_TRUE(has(t, "\"functionName\":\"GetTraderConfig\""));
}

// Кавычка в имени сделала бы файл нечитаемым для просмотрщика — молча.
TEST(Trace, NamesAreEscaped) {
    symbol_table syms;
    const auto f = sym(syms, "M", "C", "say\"hi");

    sample_log log;
    put(log, {f}, 0);

    const auto t = to_chrome_trace(log, syms, run_meta{1, 1000, 0, 0});
    EXPECT_TRUE(has(t, "say\\\"hi"));
}
