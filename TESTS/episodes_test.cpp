// Эпизоды исполнения — объективный ответ на «куда ушло время в конкретном кадре».
//
// Сводка по модам говорит про весь прогон, флеймграф — про форму стека. А вопрос
// «сервер дёрнулся, что это было» требует разбить прогон по эпизодам и показать, что
// было внутри самого долгого. Эпизод — непрерывная работа скрипта: снимки идут подряд,
// разрыв больше шага съёма означает, что скрипт останавливался.
#include <gtest/gtest.h>

#include "krprof/analysis.hpp"

using krprof::episodes;
using krprof::frame_id;
using krprof::sample_log;
using krprof::symbol;
using krprof::symbol_table;

namespace {
void put(sample_log& l, std::initializer_list<frame_id> s, std::uint64_t at) {
    l.add(std::span<const frame_id>{s.begin(), s.size()}, at);
}
frame_id put_sym(symbol_table& t, std::string func) {
    return t.intern(t.size() + 1, [&] {
        symbol s;
        s.mod = "M";
        s.klass = "C";
        s.func = std::move(func);
        return s;
    });
}
}  // namespace

TEST(Episodes, EmptyLogGivesNothing) {
    EXPECT_TRUE(episodes(sample_log{}, symbol_table{}, 1000).empty());
}

// Снимки подряд с шагом съёма — один непрерывный эпизод.
TEST(Episodes, ConsecutiveSamplesAreOneEpisode) {
    symbol_table syms;
    const auto f = put_sym(syms, "f");
    sample_log log;
    put(log, {f}, 0);
    put(log, {f}, 1000);
    put(log, {f}, 2000);

    const auto e = episodes(log, syms, 1000);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].start_us, 0u);
    EXPECT_EQ(e[0].samples, 3u);
    // Длительность считается по последнему снимку плюс его собственный шаг.
    EXPECT_EQ(e[0].dur_us, 3000u);
}

// Разрыв больше шага — скрипт останавливался, значит эпизод другой.
TEST(Episodes, GapSplitsEpisodes) {
    symbol_table syms;
    const auto f = put_sym(syms, "f");
    sample_log log;
    put(log, {f}, 0);
    put(log, {f}, 1000);
    put(log, {f}, 50000);

    const auto e = episodes(log, syms, 1000);
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].samples, 2u);
    EXPECT_EQ(e[1].start_us, 50000u);
    EXPECT_EQ(e[1].samples, 1u);
}

// Дольше — выше: длинный кадр и есть тот, ради которого смотрят.
TEST(Episodes, SortedByDurationDescending) {
    symbol_table syms;
    const auto f = put_sym(syms, "f");
    sample_log log;
    put(log, {f}, 0);                       // короткий
    put(log, {f}, 100000);                  // длинный: три снимка подряд
    put(log, {f}, 101000);
    put(log, {f}, 102000);

    const auto e = episodes(log, syms, 1000);
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0].samples, 3u);
    EXPECT_EQ(e[1].samples, 1u);
}

// Внутри эпизода нужен ответ «что именно там было»: самый частый лист.
TEST(Episodes, ReportsDominantLeaf) {
    symbol_table syms;
    const auto a = put_sym(syms, "cheap");
    const auto b = put_sym(syms, "hot");
    sample_log log;
    put(log, {a}, 0);
    put(log, {b}, 1000);
    put(log, {b}, 2000);

    const auto e = episodes(log, syms, 1000);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].top, b);
    EXPECT_EQ(e[0].top_samples, 2u);
}
