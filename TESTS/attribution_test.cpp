// Атрибуция: из пути исходника, который движок положил в DebugInfo, достаём имя мода.
//
// ПУТИ ЗДЕСЬ НАСТОЯЩИЕ. Все формы ниже сняты с живого сервера (DayZDiag, 15 модов) из
// RPT-лога — движок печатает в нём путь и строку на каждом предупреждении компилятора.
// Первая версия этой функции брала первый сегмент пути и на реальных данных оказалась
// неверной: у `KR/diag/KR_GUARANA/...` мод не `KR`.
#include <gtest/gtest.h>

#include "krprof/attribution.hpp"

using krprof::mod_of;
using krprof::prefix_of;
using krprof::unknown_mod;
using krprof::vanilla_mod;

// ── Ваниль ───────────────────────────────────────────────────────────────────
// Лежит в корне: `scripts/<слой>/...`, своего префикса нет.
TEST(Attribution, VanillaPathIsVanilla) {
    EXPECT_EQ(mod_of("scripts/4_World/entities/manbase/playerbase.c"), vanilla_mod);
    EXPECT_EQ(mod_of("scripts/5_Mission/mission/missionserver.c"), vanilla_mod);
}

// ── Односегментный префикс ───────────────────────────────────────────────────
TEST(Attribution, SimpleModPrefix) {
    EXPECT_EQ(mod_of("CodeLock/scripts/3_game/dayzgame.c"), "CodeLock");
    EXPECT_EQ(mod_of("CodeLock/scripts/4_world/entities/codelock.c"), "CodeLock");
}

// ── Многосегментный префикс: мод — ПОСЛЕДНИЙ сегмент перед `scripts` ─────────
// Ровно то, на чём сломалось правило «первый сегмент».
TEST(Attribution, NestedPrefixTakesSegmentBeforeScripts) {
    EXPECT_EQ(mod_of("KR/diag/KR_GUARANA/scripts/BASE/4_World/shapes/shapes.c"), "KR_GUARANA");
    EXPECT_EQ(mod_of("KR/nexus/KR_NEXUS/scripts/3_Game/net_test.c"), "KR_NEXUS");
    EXPECT_EQ(mod_of("KR/radios/KR_RADIOS_SERVER/scripts/4_World/printradioscfg.c"),
              "KR_RADIOS_SERVER");
    EXPECT_EQ(mod_of("KR/stalker/player_lookup/KR_PLAYER_LOOKUP/scripts/3_Game/ui/markerbracket.c"),
              "KR_PLAYER_LOOKUP");
}

// ── Без сегмента `scripts` ───────────────────────────────────────────────────
// Часть модов кладёт слой сразу под свой префикс. Тогда меткой служит сам слой.
TEST(Attribution, LayerDirectoryWorksAsMarkerToo) {
    EXPECT_EQ(mod_of("MagicByteSharedLibrary/5_Mission/mission/mbsl_missiongameplay.c"),
              "MagicByteSharedLibrary");
}

// ИЗВЕСТНАЯ НЕТОЧНОСТЬ, зафиксированная намеренно.
//
// `KR/core/KR_CORE/MATH/2_GameLib/...` — префикс PBO здесь `KR/core/KR_CORE`, а `MATH`
// уже подкаталог внутри мода. Отличить одно от другого по пути нечем: разделителя между
// «префиксом» и «подкаталогом» в строке нет, а список префиксов PBO движок наружу не
// отдаёт. Берём сегмент перед слоем и получаем `MATH` вместо `KR_CORE`.
//
// Почему это терпимо: полный префикс всё равно сохраняется (`prefix_of`) и уезжает в
// сайдкар, поэтому правда не теряется — теряется только красота короткого имени. Тест
// стоит здесь не как одобрение, а как ЗАКРЕПЛЕНИЕ: поедет поведение — покраснеет.
TEST(Attribution, KnownGap_SubfolderBeforeLayerLooksLikeMod) {
    EXPECT_EQ(mod_of("KR/core/KR_CORE/MATH/2_GameLib/easing/collection.c"), "MATH");
    // А вот полный префикс здесь верен, и по нему мод опознаётся однозначно.
    EXPECT_EQ(prefix_of("KR/core/KR_CORE/MATH/2_GameLib/easing/collection.c"),
              "KR/core/KR_CORE/MATH");
}

TEST(Attribution, PrefixIsWholePathBeforeMarker) {
    EXPECT_EQ(prefix_of("KR/diag/KR_GUARANA/scripts/BASE/4_World/shapes.c"), "KR/diag/KR_GUARANA");
    EXPECT_EQ(prefix_of("CodeLock/scripts/3_game/dayzgame.c"), "CodeLock");
    EXPECT_TRUE(prefix_of("scripts/4_World/playerbase.c").empty());
}

// ── Разделители и приставки ──────────────────────────────────────────────────
// Движок мешает прямые и обратные слэши в одном пути — так они и приходят в лог.
TEST(Attribution, MixedSeparators) {
    EXPECT_EQ(mod_of("CodeLock\\scripts\\4_world\\entities\\codelock.c"), "CodeLock");
    EXPECT_EQ(mod_of("KR/diag/KR_GUARANA/scripts/BASE/4_World/shapes\\shapes.c"), "KR_GUARANA");
}

TEST(Attribution, DriveAndLeadingSeparatorsAreStripped) {
    EXPECT_EQ(mod_of("P:\\scripts\\4_World\\playerbase.c"), vanilla_mod);
    EXPECT_EQ(mod_of("P:/CodeLock/scripts/4_world/x.c"), "CodeLock");
    EXPECT_EQ(mod_of("/scripts/4_World/playerbase.c"), vanilla_mod);
}

// ── Регистр ──────────────────────────────────────────────────────────────────
// Слои в путях встречаются и как `4_World`, и как `4_world` — в одном и том же логе.
TEST(Attribution, MarkersAreCaseInsensitive) {
    EXPECT_EQ(mod_of("Scripts/4_World/playerbase.c"), vanilla_mod);
    EXPECT_EQ(mod_of("Mod/SCRIPTS/4_World/x.c"), "Mod");
    EXPECT_EQ(mod_of("Mod/3_game/x.c"), "Mod");
    EXPECT_EQ(mod_of("Mod/3_GAME/x.c"), "Mod");
}

// Имя мода отдаётся КАК ЕСТЬ: оно уезжает в отчёт, и `codelock` вместо `CodeLock` мешает.
TEST(Attribution, ModNameKeepsItsCase) {
    EXPECT_EQ(mod_of("CodeLock/scripts/x/y.c"), "CodeLock");
}

// ── Вырожденное ──────────────────────────────────────────────────────────────
// Символизация читает чужую память, мусор там штатен. Падать нельзя.
TEST(Attribution, GarbageIsUnknownNotCrash) {
    EXPECT_EQ(mod_of(""), unknown_mod);
    EXPECT_EQ(mod_of("/"), unknown_mod);
    EXPECT_EQ(mod_of("P:"), unknown_mod);
    EXPECT_EQ(mod_of("\\\\//"), unknown_mod);
}

// Метки нет вовсе — ни `scripts`, ни слоя. Ванильный `init.c` лежит именно так.
TEST(Attribution, NoMarkerMeansVanilla) {
    EXPECT_EQ(mod_of("init.c"), vanilla_mod);
    EXPECT_EQ(mod_of("some/random/path.c"), vanilla_mod);
}

// Слой-самозванец: `4_World` первым сегментом — мода перед ним нет.
TEST(Attribution, MarkerFirstMeansVanilla) {
    EXPECT_EQ(mod_of("4_World/entities/playerbase.c"), vanilla_mod);
}
