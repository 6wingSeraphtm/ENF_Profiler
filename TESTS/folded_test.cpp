// Эмиттер свёрнутых стеков — тот самый файл, который скармливают готовому просмотрщику.
//
// Формат простой до глупости и ровно поэтому выбран: строка «кадр;кадр;кадр вес», один
// путь на строку. Его читают speedscope, inferno и flamegraph.pl без конвертации, а вся
// атрибуция уезжает прямо в ИМЯ кадра — значит видна в любом просмотрщике, и дорабатывать
// просмотрщик не надо.
#include <gtest/gtest.h>

#include "krprof/call_tree.hpp"
#include "krprof/folded.hpp"
#include "krprof/symbols.hpp"

using krprof::call_tree;
using krprof::frame_id;
using krprof::symbol;
using krprof::symbol_table;
using krprof::to_folded;

namespace {

void add(call_tree& t, std::initializer_list<frame_id> stack, std::uint64_t weight = 1) {
    t.add(std::span<const frame_id>{stack.begin(), stack.size()}, weight);
}

// Кладёт символ и возвращает его id — в тестах ключ и есть порядковый номер.
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

TEST(Folded, EmptyTreeGivesEmptyText) {
    const call_tree tree;
    const symbol_table syms;
    EXPECT_EQ(to_folded(tree, syms), "");
}

TEST(Folded, SingleFrameLine) {
    symbol_table syms;
    const auto f = put(syms, "$vanilla", "PlayerBase", "OnUpdate");

    call_tree tree;
    add(tree, {f}, 5);

    EXPECT_EQ(to_folded(tree, syms), "$vanilla|PlayerBase::OnUpdate 5\n");
}

TEST(Folded, NestedFramesAreSemicolonSeparated) {
    symbol_table syms;
    const auto outer = put(syms, "$vanilla", "MissionServer", "OnUpdate");
    const auto inner = put(syms, "TraderPlus", "PlayerBase", "OnUpdate");

    call_tree tree;
    add(tree, {outer, inner}, 3);

    EXPECT_EQ(to_folded(tree, syms),
              "$vanilla|MissionServer::OnUpdate;TraderPlus|PlayerBase::OnUpdate 3\n");
}

// Глобальная функция класса не имеет — и `::` перед ней быть не должно.
TEST(Folded, GlobalFunctionHasNoClassPrefix) {
    symbol_table syms;
    const auto f = put(syms, "TraderPlus", "", "GetTraderConfig");

    call_tree tree;
    add(tree, {f}, 2);

    EXPECT_EQ(to_folded(tree, syms), "TraderPlus|GetTraderConfig 2\n");
}

// Ровно тот случай, ради которого всё затевалось: цепочка override'ов видна как
// вложенные кадры, и у каждого свой мод. `super.OnUpdate()` кладёт НОВЫЙ кадр с
// `Funct` базы, поэтому стоимость каждого звена отделена сама собой.
TEST(Folded, OverrideChainShowsCostPerMod) {
    symbol_table syms;
    const auto modded = put(syms, "TraderPlus", "PlayerBase", "OnUpdate");
    const auto base = put(syms, "$vanilla", "PlayerBase", "OnUpdate");

    call_tree tree;
    add(tree, {modded}, 7);        // время в самом override'е, до/после super
    add(tree, {modded, base}, 20); // время внутри super

    EXPECT_EQ(to_folded(tree, syms),
              "TraderPlus|PlayerBase::OnUpdate 7\n"
              "TraderPlus|PlayerBase::OnUpdate;$vanilla|PlayerBase::OnUpdate 20\n");
}

TEST(Folded, ManyLinesAreDeterministicallyOrdered) {
    symbol_table syms;
    const auto a = put(syms, "A", "C", "f");
    const auto b = put(syms, "B", "C", "f");

    call_tree tree;
    add(tree, {b}, 1);
    add(tree, {a}, 2);

    EXPECT_EQ(to_folded(tree, syms), "A|C::f 2\nB|C::f 1\n");
}

// ── Санитайзинг ──────────────────────────────────────────────────────────────
// В формате `;` разделяет кадры, а пробел отделяет вес. Имена приходят из чужой памяти,
// и один такой символ в имени сделает файл не просто кривым, а МОЛЧА неверным: строка
// разберётся, но не так. Поэтому режем, а не надеемся.
TEST(Folded, SeparatorsInsideNamesAreNeutralised) {
    symbol_table syms;
    const auto f = put(syms, "we;ird", "cl ass", "fu;nc");

    call_tree tree;
    add(tree, {f}, 1);

    EXPECT_EQ(to_folded(tree, syms), "we_ird|cl_ass::fu_nc 1\n");
}

TEST(Folded, NewlinesInsideNamesAreNeutralised) {
    symbol_table syms;
    const auto f = put(syms, "m\nod", "C", "f\r\ng");

    call_tree tree;
    add(tree, {f}, 1);

    EXPECT_EQ(to_folded(tree, syms), "m_od|C::f__g 1\n");
}

// Кадр, который не удалось разобрать, обязан остаться видимым и не сломать формат.
TEST(Folded, UnresolvedFrameIsNamedNotBlank) {
    symbol_table syms;
    const auto f = syms.intern(1, [] { return symbol{}; });

    call_tree tree;
    add(tree, {f}, 4);

    EXPECT_EQ(to_folded(tree, syms), "$unknown|? 4\n");
}

// Кадр, которого в таблице нет вовсе (id из дерева пережил clear таблицы).
TEST(Folded, FrameIdOutsideTableStillEmitsALine) {
    const symbol_table syms;
    call_tree tree;
    add(tree, {777}, 1);

    EXPECT_EQ(to_folded(tree, syms), "$unknown|? 1\n");
}
