// Сайдкар: то, что в имя кадра не влезло.
//
// Свёрнутые стеки несут форму флеймграфа, но не несут ни метаданных прогона, ни сводки
// «сколько ест каждый мод» — а это и есть тот ответ, ради которого профайлер писался.
// Сводка складывается из СОБСТВЕННОГО веса кадров: время, ушедшее в super или в чужой
// вызов, принадлежит тому, кто его исполнял, а не тому, кто позвал.
#include <gtest/gtest.h>

#include "krprof/call_tree.hpp"
#include "krprof/sidecar.hpp"
#include "krprof/symbols.hpp"

using krprof::call_tree;
using krprof::frame_id;
using krprof::run_meta;
using krprof::symbol;
using krprof::symbol_table;
using krprof::to_sidecar_json;

namespace {

void add(call_tree& t, std::initializer_list<frame_id> stack, std::uint64_t weight = 1) {
    t.add(std::span<const frame_id>{stack.begin(), stack.size()}, weight);
}

frame_id put(symbol_table& t, std::string mod, std::string klass, std::string func) {
    return t.intern(t.size() + 1, [&] {
        symbol s;
        s.mod = std::move(mod);
        s.klass = std::move(klass);
        s.func = std::move(func);
        return s;
    });
}

// Есть ли подстрока — сравнивать JSON целиком хрупко, а проверять надо смысл.
bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST(Sidecar, EmptyRunIsStillValidJson) {
    const call_tree tree;
    const symbol_table syms;
    const auto json = to_sidecar_json(tree, syms, run_meta{});

    EXPECT_TRUE(json.starts_with("{"));
    EXPECT_TRUE(json.ends_with("}\n"));
    EXPECT_TRUE(has(json, "\"mods\": []"));
}

TEST(Sidecar, MetaIsCarried) {
    run_meta meta;
    meta.samples = 1234;
    meta.sample_hz = 1000;
    meta.wall_ms = 5000;
    meta.dropped = 7;

    const auto json = to_sidecar_json(call_tree{}, symbol_table{}, meta);

    EXPECT_TRUE(has(json, "\"samples\": 1234"));
    EXPECT_TRUE(has(json, "\"sample_hz\": 1000"));
    EXPECT_TRUE(has(json, "\"wall_ms\": 5000"));
    EXPECT_TRUE(has(json, "\"dropped\": 7"));
}

// Сердце сайдкара: во что обошёлся каждый мод.
TEST(Sidecar, PerModTotalsSumSelfWeight) {
    symbol_table syms;
    const auto a = put(syms, "TraderPlus", "PlayerBase", "OnUpdate");
    const auto b = put(syms, "$vanilla", "PlayerBase", "OnUpdate");

    call_tree tree;
    add(tree, {a}, 30);
    add(tree, {a, b}, 70);

    const auto json = to_sidecar_json(tree, syms, run_meta{});

    EXPECT_TRUE(has(json, "\"mod\": \"TraderPlus\""));
    EXPECT_TRUE(has(json, "\"self\": 30"));
    EXPECT_TRUE(has(json, "\"mod\": \"$vanilla\""));
    EXPECT_TRUE(has(json, "\"self\": 70"));
}

// Один мод, много кадров — складывается в одну строку.
TEST(Sidecar, FramesOfSameModAreSummed) {
    symbol_table syms;
    const auto a = put(syms, "TraderPlus", "C", "f");
    const auto b = put(syms, "TraderPlus", "C", "g");

    call_tree tree;
    add(tree, {a}, 5);
    add(tree, {b}, 6);

    const auto json = to_sidecar_json(tree, syms, run_meta{});
    EXPECT_TRUE(has(json, "\"self\": 11"));
    // Ровно одна запись про этот мод.
    EXPECT_EQ(json.find("TraderPlus"), json.rfind("TraderPlus"));
}

// Порядок — по убыванию стоимости: самый дорогой мод читают первым.
TEST(Sidecar, ModsAreSortedByCostDescending) {
    symbol_table syms;
    const auto cheap = put(syms, "Cheap", "C", "f");
    const auto pricey = put(syms, "Pricey", "C", "f");

    call_tree tree;
    add(tree, {cheap}, 1);
    add(tree, {pricey}, 99);

    const auto json = to_sidecar_json(tree, syms, run_meta{});
    EXPECT_LT(json.find("Pricey"), json.find("Cheap"));
}

// Доля в процентах: без неё абсолютные веса ни о чём не говорят.
TEST(Sidecar, ShareIsPercentOfTotal) {
    symbol_table syms;
    const auto a = put(syms, "Quarter", "C", "f");
    const auto b = put(syms, "Rest", "C", "f");

    call_tree tree;
    add(tree, {a}, 25);
    add(tree, {b}, 75);

    const auto json = to_sidecar_json(tree, syms, run_meta{});
    EXPECT_TRUE(has(json, "\"share\": 25.00"));
    EXPECT_TRUE(has(json, "\"share\": 75.00"));
}

// ── Экранирование ────────────────────────────────────────────────────────────
// Имена приходят из чужой памяти. Кавычка или обратный слэш внутри имени превращают
// сайдкар в невалидный JSON, и отчёт молча перестаёт открываться.
TEST(Sidecar, QuotesAndBackslashesAreEscaped) {
    symbol_table syms;
    const auto f = put(syms, "we\"ird\\mod", "C", "f");

    call_tree tree;
    add(tree, {f}, 1);

    const auto json = to_sidecar_json(tree, syms, run_meta{});
    EXPECT_TRUE(has(json, "\"mod\": \"we\\\"ird\\\\mod\""));
}

TEST(Sidecar, ControlCharactersAreEscaped) {
    symbol_table syms;
    const auto f = put(syms, "bad\nmod\t", "C", "f");

    call_tree tree;
    add(tree, {f}, 1);

    const auto json = to_sidecar_json(tree, syms, run_meta{});
    EXPECT_TRUE(has(json, "\\n"));
    EXPECT_TRUE(has(json, "\\t"));
    EXPECT_FALSE(has(json, "bad\nmod"));
}
