// Свёртки для отчёта: что дорого и кто кого переопределил.
//
// Флеймграф отвечает на вопрос «какой формы был стек», а спрашивают обычно другое:
// сколько стоит функция и сколько стоит мод. В флеймграфе это надо выискивать глазами
// среди пяти одинаковых `DayZGame::OnUpdate`, поэтому свёртки считаются отдельно и
// показываются таблицей.
#include <gtest/gtest.h>

#include "krprof/analysis.hpp"

using krprof::by_function;
using krprof::call_tree;
using krprof::frame_id;
using krprof::override_chains;
using krprof::symbol;
using krprof::symbol_table;

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

}  // namespace

// ── Стоимость функций ────────────────────────────────────────────────────────
TEST(Analysis, EmptyTreeGivesNothing) {
    EXPECT_TRUE(by_function(call_tree{}, symbol_table{}).empty());
}

// Один кадр может стоять в дереве в разных местах — его собственный вес складывается.
TEST(Analysis, SelfWeightIsSummedAcrossPositions) {
    symbol_table syms;
    const auto outer = put(syms, "M", "C", "outer");
    const auto leaf = put(syms, "M", "C", "leaf");

    call_tree tree;
    add(tree, {outer, leaf}, 10);
    add(tree, {leaf}, 5);

    const auto out = by_function(tree, syms);
    ASSERT_EQ(out.size(), 1u);  // у outer собственного веса нет
    EXPECT_EQ(out[0].id, leaf);
    EXPECT_EQ(out[0].self, 15u);
}

// Дороже — выше: таблицу читают сверху.
TEST(Analysis, SortedByCostDescending) {
    symbol_table syms;
    const auto cheap = put(syms, "M", "C", "cheap");
    const auto pricey = put(syms, "M", "C", "pricey");

    call_tree tree;
    add(tree, {cheap}, 1);
    add(tree, {pricey}, 99);

    const auto out = by_function(tree, syms);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].id, pricey);
    EXPECT_EQ(out[1].id, cheap);
}

// ── Цепочки override'ов ──────────────────────────────────────────────────────
// Ради этого всё и затевалось. В флеймграфе пять `DayZGame::OnUpdate` — пять почти
// одинаковых полосок, и чья из них чья, глазом не разобрать. Здесь это одна строка на
// мод с его собственной ценой.
TEST(Analysis, OverrideChainGroupsSameMethodAcrossMods) {
    symbol_table syms;
    const auto a = put(syms, "TraderPlus", "PlayerBase", "OnUpdate");
    const auto b = put(syms, "CF", "PlayerBase", "OnUpdate");
    const auto c = put(syms, "$vanilla", "PlayerBase", "OnUpdate");

    call_tree tree;
    add(tree, {a}, 30);
    add(tree, {a, b}, 20);
    add(tree, {a, b, c}, 50);

    const auto chains = override_chains(tree, syms);
    ASSERT_EQ(chains.size(), 1u);
    EXPECT_EQ(chains[0].name, "PlayerBase::OnUpdate");
    EXPECT_EQ(chains[0].total, 100u);

    ASSERT_EQ(chains[0].links.size(), 3u);
    // Внутри цепочки — тоже по убыванию цены.
    EXPECT_EQ(chains[0].links[0].mod, "$vanilla");
    EXPECT_EQ(chains[0].links[0].self, 50u);
    EXPECT_EQ(chains[0].links[1].mod, "TraderPlus");
    EXPECT_EQ(chains[0].links[1].self, 30u);
    EXPECT_EQ(chains[0].links[2].mod, "CF");
    EXPECT_EQ(chains[0].links[2].self, 20u);
}

// Один мод — это не цепочка, а обычная функция. Показывать её как «переопределение»
// значило бы утопить настоящие цепочки в шуме.
TEST(Analysis, SingleModIsNotAChain) {
    symbol_table syms;
    const auto only = put(syms, "M", "C", "f");

    call_tree tree;
    add(tree, {only}, 10);

    EXPECT_TRUE(override_chains(tree, syms).empty());
}

// Разные методы не смешиваются, даже если моды те же.
TEST(Analysis, DifferentMethodsAreDifferentChains) {
    symbol_table syms;
    const auto a1 = put(syms, "A", "C", "f");
    const auto b1 = put(syms, "B", "C", "f");
    const auto a2 = put(syms, "A", "C", "g");
    const auto b2 = put(syms, "B", "C", "g");

    call_tree tree;
    add(tree, {a1}, 5);
    add(tree, {b1}, 5);
    add(tree, {a2}, 100);
    add(tree, {b2}, 1);

    const auto chains = override_chains(tree, syms);
    ASSERT_EQ(chains.size(), 2u);
    // Дороже — выше и здесь.
    EXPECT_EQ(chains[0].name, "C::g");
    EXPECT_EQ(chains[0].total, 101u);
    EXPECT_EQ(chains[1].name, "C::f");
    EXPECT_EQ(chains[1].total, 10u);
}

// Один и тот же мод может стоять в цепочке дважды (два его файла правят один метод) —
// складываем, иначе строка задвоится и проценты поедут.
TEST(Analysis, SameModTwiceInChainIsSummed) {
    symbol_table syms;
    const auto a = put(syms, "KR_CORE", "DayZGame", "OnUpdate");
    const auto b = put(syms, "KR_CORE", "DayZGame", "OnUpdate");
    const auto v = put(syms, "$vanilla", "DayZGame", "OnUpdate");

    call_tree tree;
    add(tree, {a}, 7);
    add(tree, {b}, 3);
    add(tree, {v}, 1);

    const auto chains = override_chains(tree, syms);
    ASSERT_EQ(chains.size(), 1u);
    ASSERT_EQ(chains[0].links.size(), 2u);
    EXPECT_EQ(chains[0].links[0].mod, "KR_CORE");
    EXPECT_EQ(chains[0].links[0].self, 10u);
}

// Глобальная функция класса не имеет — цепочкой быть не может.
TEST(Analysis, GlobalFunctionsAreNotChains) {
    symbol_table syms;
    const auto a = put(syms, "A", "", "GlobalFn");
    const auto b = put(syms, "B", "", "GlobalFn");

    call_tree tree;
    add(tree, {a}, 5);
    add(tree, {b}, 5);

    EXPECT_TRUE(override_chains(tree, syms).empty());
}
