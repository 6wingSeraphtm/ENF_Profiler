// Схлопывание подкаталогов мода в сам мод.
//
// `mod_of` смотрит на ОДИН путь и потому не может отличить подкаталог мода от его
// префикса: у `KR/core/KR_CORE/MATH/2_GameLib/...` перед слоем стоит `MATH`, и по одному
// пути это неотличимо от настоящего мода. На живом сервере это дало четыре «мода»
// (MATH, CORO, EVENT, UI) там, где мод один — KR_CORE.
//
// Различие видно только по НАБОРУ путей: если несколько разных имён сидят под общим
// родителем, значит родитель и есть мод, а они — его части. Поэтому правило работает над
// множеством, а не над строкой, и живёт отдельно от `mod_of`.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "krprof/modmap.hpp"

using krprof::mod_map;

namespace {

mod_map built(std::vector<std::string> paths) {
    mod_map m;
    for (const std::string& p : paths) { m.observe(p); }
    m.finish();
    return m;
}

}  // namespace

// Пустая карта ничего не схлопывает и потому отвечает ровно то же, что обычное правило.
// Схлопывание — это уточнение поверх `mod_of`, а не замена ему.
TEST(ModMap, EmptyMapFallsBackToPlainRule) {
    mod_map m;
    m.finish();
    EXPECT_EQ(m.of("whatever/4_World/x.c"), "whatever");
    EXPECT_EQ(m.of("scripts/4_World/x.c"), krprof::vanilla_mod);
    EXPECT_EQ(m.of(""), krprof::unknown_mod);
}

// Один мод — ничего схлопывать не надо.
TEST(ModMap, SingleModStaysAsIs) {
    const auto m = built({"MagicByteSharedLibrary/3_Game/core/mbsl_dayzgame.c"});
    EXPECT_EQ(m.of("MagicByteSharedLibrary/3_Game/core/mbsl_dayzgame.c"),
              "MagicByteSharedLibrary");
}

TEST(ModMap, VanillaStaysVanilla) {
    const auto m = built({"scripts/3_Game/dayzgame.c", "scripts/4_World/playerbase.c"});
    EXPECT_EQ(m.of("scripts/3_Game/dayzgame.c"), krprof::vanilla_mod);
}

// Ради этого всё и написано: четыре подкаталога одного мода становятся одним модом.
// Пути настоящие, сняты с живого сервера.
TEST(ModMap, SubfoldersOfOneModCollapse) {
    const auto m = built({
        "KR/core/KR_CORE/MATH/2_GameLib/easing/collection.c",
        "KR/core/KR_CORE/CORO/3_Game/startreactor.c",
        "KR/core/KR_CORE/EVENT/3_Game/sources/source_dayzgame.c",
        "KR/core/KR_CORE/UI/4_World/widget.c",
    });

    EXPECT_EQ(m.of("KR/core/KR_CORE/MATH/2_GameLib/easing/collection.c"), "KR_CORE");
    EXPECT_EQ(m.of("KR/core/KR_CORE/CORO/3_Game/startreactor.c"), "KR_CORE");
    EXPECT_EQ(m.of("KR/core/KR_CORE/EVENT/3_Game/sources/source_dayzgame.c"), "KR_CORE");
    EXPECT_EQ(m.of("KR/core/KR_CORE/UI/4_World/widget.c"), "KR_CORE");
}

// Два разных мода под общим каталогом-складом НЕ схлопываются: у каждого свой `scripts`,
// то есть каждый сам по себе полноценный префикс.
TEST(ModMap, DistinctModsUnderCommonShelfStaySeparate) {
    const auto m = built({
        "KR/diag/KR_GUARANA/scripts/BASE/4_World/shapes.c",
        "KR/nexus/KR_NEXUS/scripts/3_Game/net_test.c",
    });

    EXPECT_EQ(m.of("KR/diag/KR_GUARANA/scripts/BASE/4_World/shapes.c"), "KR_GUARANA");
    EXPECT_EQ(m.of("KR/nexus/KR_NEXUS/scripts/3_Game/net_test.c"), "KR_NEXUS");
}

// Мод с сегментом `scripts` схлопывать нельзя даже при общем родителе: `scripts` и есть
// граница мода, она сказана явно.
TEST(ModMap, ScriptsMarkerPinsTheModAndBlocksCollapse) {
    const auto m = built({
        "JM/CF/Scripts/3_Game/communityframework/game/dayzgame.c",
        "JM/CF/Scripts/4_World/x.c",
    });
    EXPECT_EQ(m.of("JM/CF/Scripts/3_Game/communityframework/game/dayzgame.c"), "CF");
}

// Смешанный случай: рядом и схлопываемый мод, и обычные.
TEST(ModMap, MixedSetIsHandledPerMod) {
    const auto m = built({
        "KR/core/KR_CORE/MATH/2_GameLib/easing/collection.c",
        "KR/core/KR_CORE/CORO/3_Game/startreactor.c",
        "MagicByteSharedLibrary/3_Game/core/mbsl_dayzgame.c",
        "scripts/3_Game/dayzgame.c",
        "KR/diag/KR_GUARANA/scripts/BASE/4_World/shapes.c",
    });

    EXPECT_EQ(m.of("KR/core/KR_CORE/MATH/2_GameLib/easing/collection.c"), "KR_CORE");
    EXPECT_EQ(m.of("KR/core/KR_CORE/CORO/3_Game/startreactor.c"), "KR_CORE");
    EXPECT_EQ(m.of("MagicByteSharedLibrary/3_Game/core/mbsl_dayzgame.c"),
              "MagicByteSharedLibrary");
    EXPECT_EQ(m.of("scripts/3_Game/dayzgame.c"), krprof::vanilla_mod);
    EXPECT_EQ(m.of("KR/diag/KR_GUARANA/scripts/BASE/4_World/shapes.c"), "KR_GUARANA");
}

// Один подкаталог — не повод схлопывать: может, мод и правда так называется.
TEST(ModMap, LoneSubfolderIsNotCollapsed) {
    const auto m = built({"Some/Where/ONLYONE/3_Game/x.c"});
    EXPECT_EQ(m.of("Some/Where/ONLYONE/3_Game/x.c"), "ONLYONE");
}

// Незнакомый путь после finish() — не повод падать.
TEST(ModMap, UnseenPathFallsBackToPlainRule) {
    const auto m = built({"scripts/3_Game/dayzgame.c"});
    EXPECT_EQ(m.of("NewMod/4_World/x.c"), "NewMod");
}

TEST(ModMap, GarbagePathIsUnknown) {
    const auto m = built({"scripts/3_Game/x.c"});
    EXPECT_EQ(m.of(""), krprof::unknown_mod);
}
