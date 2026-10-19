// Таблица символов: перевод «указатель движка» -> «мод, класс, функция, файл».
//
// Зачем отдельная таблица, а не разбор на месте: символизация — самое дорогое, что есть
// в профайлере (обход отладочной информации движка, разбор пути, поиск объявляющего
// класса). Делать её на КАЖДОМ снимке нельзя, а на каждый уникальный кадр — можно, их
// тысячи, а снимков миллионы. Отсюда мемоизация по ключу.
#include <gtest/gtest.h>

#include "krprof/symbols.hpp"

using krprof::symbol;
using krprof::symbol_table;

namespace {

symbol make(std::string func) {
    symbol s;
    s.mod = "TraderPlus";
    s.klass = "PlayerBase";
    s.func = std::move(func);
    s.file = "TraderPlus/scripts/4_World/playerbase.c";
    s.line = 42;
    return s;
}

}  // namespace

TEST(Symbols, EmptyTableHasNoSymbols) {
    const symbol_table t;
    EXPECT_EQ(t.size(), 0u);
}

TEST(Symbols, InternReturnsUsableId) {
    symbol_table t;
    const auto id = t.intern(0xABC, [] { return make("OnUpdate"); });

    EXPECT_EQ(t.size(), 1u);
    EXPECT_EQ(t[id].func, "OnUpdate");
    EXPECT_EQ(t[id].mod, "TraderPlus");
    EXPECT_EQ(t[id].line, 42u);
}

// Главное свойство: тот же ключ — тот же id, и разбор больше не повторяется.
TEST(Symbols, SameKeyReturnsSameIdAndResolvesOnce) {
    symbol_table t;
    int calls = 0;
    const auto make_counted = [&calls] {
        ++calls;
        return make("OnUpdate");
    };

    const auto a = t.intern(0xABC, make_counted);
    const auto b = t.intern(0xABC, make_counted);

    EXPECT_EQ(a, b);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(t.size(), 1u);
}

TEST(Symbols, DifferentKeysGetDifferentIds) {
    symbol_table t;
    const auto a = t.intern(1, [] { return make("OnUpdate"); });
    const auto b = t.intern(2, [] { return make("EEInit"); });

    EXPECT_NE(a, b);
    EXPECT_EQ(t.size(), 2u);
    EXPECT_EQ(t[a].func, "OnUpdate");
    EXPECT_EQ(t[b].func, "EEInit");
}

// Ключи приходят из движка (адреса `Funct` и класса) — они разреженные и большие.
TEST(Symbols, SparseEngineLikeKeysWork) {
    symbol_table t;
    const auto a = t.intern(0x7FF6'1234'5678ull, [] { return make("A"); });
    const auto b = t.intern(0x7FF6'1234'5680ull, [] { return make("B"); });

    EXPECT_NE(a, b);
    EXPECT_EQ(t[a].func, "A");
    EXPECT_EQ(t[b].func, "B");
}

// Символизация читает чужую память, и провал там — штатный исход, а не повод падать.
// Неизвестный кадр обязан остаться В ОТЧЁТЕ: дырка в флеймграфе врёт сильнее, чем
// честная строка «неизвестно».
TEST(Symbols, UnknownSymbolIsStillASymbol) {
    symbol_table t;
    const auto id = t.intern(0, [] { return symbol{}; });

    EXPECT_EQ(t.size(), 1u);
    EXPECT_TRUE(t[id].func.empty());
}

// Выход за границы не должен быть UB: id уезжает в дерево, а дерево живёт своей жизнью.
TEST(Symbols, OutOfRangeIdGivesPlaceholderNotCrash) {
    const symbol_table t;
    EXPECT_TRUE(t[12345].func.empty());
    EXPECT_TRUE(t[12345].mod.empty());
}

TEST(Symbols, ClearResetsTable) {
    symbol_table t;
    t.intern(1, [] { return make("A"); });
    t.clear();
    EXPECT_EQ(t.size(), 0u);
}
