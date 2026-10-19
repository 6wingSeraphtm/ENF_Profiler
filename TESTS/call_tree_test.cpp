// Дерево стеков: то, чего у ванильного профайлера нет и из-за чего флеймграф из него
// не строится. Сюда сэмплер складывает снятые стеки, отсюда их забирает эмиттер.
//
// Кадры здесь — просто числа (`frame_id`). Дерево не знает ни про функции, ни про моды,
// ни про движок: это чистая структура, и потому проверяется целиком без игры.
#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

#include "krprof/call_tree.hpp"

using krprof::call_tree;
using krprof::frame_id;

namespace {

// Стек пишется КАК В ДВИЖКЕ: снаружи внутрь, `stack[0]` — самый внешний кадр.
// Массив кадров скриптового потока лежит ровно так, и разворачивать его незачем.
void add(call_tree& t, std::initializer_list<frame_id> stack, std::uint64_t weight = 1) {
    t.add(std::span<const frame_id>{stack.begin(), stack.size()}, weight);
}

std::vector<krprof::folded_entry> flat(const call_tree& t) { return t.flatten(); }

}  // namespace

TEST(CallTree, EmptyTreeIsEmpty) {
    const call_tree t;
    EXPECT_EQ(t.total(), 0u);
    EXPECT_TRUE(flat(t).empty());
}

TEST(CallTree, SingleSampleBecomesSingleLeaf) {
    call_tree t;
    add(t, {1, 2, 3});

    const auto out = flat(t);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].path, (std::vector<frame_id>{1, 2, 3}));
    EXPECT_EQ(out[0].self, 1u);
    EXPECT_EQ(t.total(), 1u);
}

// Ради этого дерево и существует: одинаковые стеки схлопываются, а не копятся списком.
TEST(CallTree, IdenticalStacksMerge) {
    call_tree t;
    add(t, {1, 2});
    add(t, {1, 2});
    add(t, {1, 2});

    const auto out = flat(t);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].self, 3u);
    EXPECT_EQ(t.total(), 3u);
}

TEST(CallTree, WeightIsAccumulatedNotCounted) {
    call_tree t;
    add(t, {1}, 40);
    add(t, {1}, 2);

    const auto out = flat(t);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].self, 42u);
    EXPECT_EQ(t.total(), 42u);
}

// Общий префикс делится, дальше расходится: это и есть форма флеймграфа.
TEST(CallTree, CommonPrefixIsShared) {
    call_tree t;
    add(t, {1, 2});
    add(t, {1, 3});

    const auto out = flat(t);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].path, (std::vector<frame_id>{1, 2}));
    EXPECT_EQ(out[1].path, (std::vector<frame_id>{1, 3}));
    // Узлов три: общий корень 1 и две ветки под ним — корень не задвоился.
    EXPECT_EQ(t.nodes(), 3u);
}

// Тонкое место. Стек `A` — это НЕ «начало стека `A;B`»: он означает, что в момент
// снимка мы были в A и ни в чьём вызове. Такой узел обязан иметь собственный вес, иначе
// self-time внешних функций утечёт детям и флеймграф соврёт.
TEST(CallTree, PrefixStackKeepsItsOwnSelfWeight) {
    call_tree t;
    add(t, {1});
    add(t, {1, 2});

    const auto out = flat(t);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].path, (std::vector<frame_id>{1}));
    EXPECT_EQ(out[0].self, 1u);
    EXPECT_EQ(out[1].path, (std::vector<frame_id>{1, 2}));
    EXPECT_EQ(out[1].self, 1u);
    EXPECT_EQ(t.total(), 2u);
}

// Рекурсия: тот же кадр на разной глубине — разные узлы. Ванильный профайлер здесь
// двоит время в один счётчик `Funct::Time`; дерево различает уровни.
TEST(CallTree, RecursionMakesDistinctNodesPerDepth) {
    call_tree t;
    add(t, {7, 7, 7});

    const auto out = flat(t);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].path, (std::vector<frame_id>{7, 7, 7}));
    EXPECT_EQ(t.nodes(), 3u);
}

// Разные корни не смешиваются.
TEST(CallTree, DistinctRootsStaySeparate) {
    call_tree t;
    add(t, {1, 9});
    add(t, {2, 9});

    const auto out = flat(t);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].path, (std::vector<frame_id>{1, 9}));
    EXPECT_EQ(out[1].path, (std::vector<frame_id>{2, 9}));
    EXPECT_EQ(t.nodes(), 4u);
}

// Пустой стек класть нечего, но и падать не за что: сэмплер ловит поток и в момент,
// когда скрипт не исполняется вовсе, и это штатный исход, а не ошибка.
TEST(CallTree, EmptyStackIsIgnored) {
    call_tree t;
    add(t, {});
    EXPECT_EQ(t.total(), 0u);
    EXPECT_TRUE(flat(t).empty());
}

TEST(CallTree, ZeroWeightDoesNotCreateLeaf) {
    call_tree t;
    add(t, {1, 2}, 0);
    EXPECT_EQ(t.total(), 0u);
    EXPECT_TRUE(flat(t).empty());
}

// Вывод обязан быть детерминированным: отчёты сравнивают между прогонами, и
// «поменялся порядок строк» — это шум, в котором тонет настоящая разница.
TEST(CallTree, FlattenIsDeterministicallyOrdered) {
    call_tree a;
    add(a, {2});
    add(a, {1, 5});
    add(a, {1, 4});

    call_tree b;
    add(b, {1, 4});
    add(b, {2});
    add(b, {1, 5});

    EXPECT_EQ(flat(a), flat(b));

    const auto out = flat(a);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].path, (std::vector<frame_id>{1, 4}));
    EXPECT_EQ(out[1].path, (std::vector<frame_id>{1, 5}));
    EXPECT_EQ(out[2].path, (std::vector<frame_id>{2}));
}

// Глубокий стек: у движка предел кадров свой, но дерево ограничивать нечем и незачем.
TEST(CallTree, DeepStackSurvives) {
    std::vector<frame_id> deep(512);
    for (frame_id i = 0; i < 512; ++i) { deep[i] = i; }

    call_tree t;
    t.add(deep, 5);

    const auto out = flat(t);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].path.size(), 512u);
    EXPECT_EQ(out[0].self, 5u);
    EXPECT_EQ(t.nodes(), 512u);
}

TEST(CallTree, ClearResetsEverything) {
    call_tree t;
    add(t, {1, 2}, 10);
    t.clear();

    EXPECT_EQ(t.total(), 0u);
    EXPECT_EQ(t.nodes(), 0u);
    EXPECT_TRUE(flat(t).empty());
}
