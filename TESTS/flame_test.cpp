// Дерево для флеймграфа: то же дерево вызовов, но с СУММАРНЫМ весом на каждом узле.
//
// Свёрнутые стеки несут только собственный вес листа — этого хватает эмиттерам, но не
// отрисовке: ширина полоски во флеймграфе это вес всего поддерева. Считаем один раз
// здесь, чтобы браузер получил готовые числа и не пересчитывал их на каждом кадре.
#include <gtest/gtest.h>

#include "krprof/analysis.hpp"

using krprof::call_tree;
using krprof::flame_tree;
using krprof::frame_id;

namespace {
void add(call_tree& t, std::initializer_list<frame_id> s, std::uint64_t w = 1) {
    t.add(std::span<const frame_id>{s.begin(), s.size()}, w);
}
}  // namespace

// У флеймграфа должно быть одно основание, даже когда корней в дереве несколько.
TEST(Flame, AlwaysHasSyntheticRoot) {
    const auto f = flame_tree(call_tree{});
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].total, 0u);
    EXPECT_TRUE(f[0].children.empty());
    EXPECT_TRUE(f[0].is_root);
}

TEST(Flame, TotalFlowsUpFromLeaf) {
    call_tree t;
    add(t, {1, 2}, 5);

    const auto f = flame_tree(t);
    ASSERT_EQ(f.size(), 3u);
    EXPECT_EQ(f[0].total, 5u);                  // основание
    ASSERT_EQ(f[0].children.size(), 1u);

    const auto& outer = f[f[0].children[0]];
    EXPECT_EQ(outer.id, 1u);
    EXPECT_EQ(outer.total, 5u);
    EXPECT_EQ(outer.self, 0u);                  // сам ничего не исполнял

    const auto& leaf = f[outer.children[0]];
    EXPECT_EQ(leaf.id, 2u);
    EXPECT_EQ(leaf.total, 5u);
    EXPECT_EQ(leaf.self, 5u);
}

// Кадр, который и сам работал, и звал других: свой вес и вес детей складываются.
TEST(Flame, SelfAndChildrenBothCountTowardTotal) {
    call_tree t;
    add(t, {1}, 30);
    add(t, {1, 2}, 70);

    const auto f = flame_tree(t);
    const auto& outer = f[f[0].children[0]];
    EXPECT_EQ(outer.self, 30u);
    EXPECT_EQ(outer.total, 100u);
    EXPECT_EQ(f[outer.children[0]].total, 70u);
}

TEST(Flame, SharedPrefixBranches) {
    call_tree t;
    add(t, {1, 2}, 3);
    add(t, {1, 3}, 7);

    const auto f = flame_tree(t);
    const auto& outer = f[f[0].children[0]];
    EXPECT_EQ(outer.total, 10u);
    ASSERT_EQ(outer.children.size(), 2u);
}

TEST(Flame, DistinctRootsSitSideBySide) {
    call_tree t;
    add(t, {1}, 4);
    add(t, {2}, 6);

    const auto f = flame_tree(t);
    EXPECT_EQ(f[0].total, 10u);
    EXPECT_EQ(f[0].children.size(), 2u);
}

// Дети идут по убыванию веса: широкое слева читается первым, и порядок не зависит от
// обхода хеш-таблицы — иначе два прогона дали бы разные картинки на одних данных.
TEST(Flame, ChildrenSortedByWeightDescending) {
    call_tree t;
    add(t, {1, 9}, 1);
    add(t, {1, 8}, 50);
    add(t, {1, 7}, 10);

    const auto f = flame_tree(t);
    const auto& outer = f[f[0].children[0]];
    ASSERT_EQ(outer.children.size(), 3u);
    EXPECT_EQ(f[outer.children[0]].id, 8u);
    EXPECT_EQ(f[outer.children[1]].id, 7u);
    EXPECT_EQ(f[outer.children[2]].id, 9u);
}

TEST(Flame, DepthIsRecorded) {
    call_tree t;
    add(t, {1, 2, 3}, 1);

    const auto f = flame_tree(t);
    EXPECT_EQ(f[0].depth, 0u);
    const auto& a = f[f[0].children[0]];
    EXPECT_EQ(a.depth, 1u);
    EXPECT_EQ(f[f[a.children[0]].children[0]].depth, 3u);
}
