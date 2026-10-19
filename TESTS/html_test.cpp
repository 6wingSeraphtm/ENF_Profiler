// Дымовые проверки отчёта. Вёрстку тестами не ловят — ловят то, что страница осталась
// целой и несёт факты: без этого молча сломанный отчёт заметят только глазами.
#include <gtest/gtest.h>

#include "krprof/html.hpp"
#include "krprof/sample_log.hpp"

using krprof::call_tree;
using krprof::frame_id;
using krprof::run_meta;
using krprof::symbol;
using krprof::symbol_table;
using krprof::to_html;

namespace {

void add(call_tree& t, std::initializer_list<frame_id> stack, std::uint64_t w = 1) {
    t.add(std::span<const frame_id>{stack.begin(), stack.size()}, w);
}

frame_id put(symbol_table& t, std::string mod, std::string klass, std::string func) {
    return t.intern(t.size() + 1, [&] {
        symbol s;
        s.mod = std::move(mod);
        s.klass = std::move(klass);
        s.func = std::move(func);
        s.file = "M/scripts/4_World/x.c";
        s.line = 42;
        return s;
    });
}

bool has(const std::string& h, std::string_view n) { return h.find(n) != std::string::npos; }

}  // namespace

TEST(Html, EmptyRunIsStillAWholePage) {
    const auto page = to_html(call_tree{}, krprof::sample_log{}, symbol_table{}, run_meta{});
    EXPECT_TRUE(page.starts_with("<!doctype html>"));
    EXPECT_TRUE(page.ends_with("</html>\n"));
}

TEST(Html, ModsAndFunctionsAppear) {
    symbol_table syms;
    const auto a = put(syms, "TraderPlus", "PlayerBase", "OnUpdate");
    call_tree tree;
    add(tree, {a}, 10);

    const auto page = to_html(tree, krprof::sample_log{}, syms, run_meta{10, 500, 1000, 0});
    EXPECT_TRUE(has(page, "TraderPlus"));
    EXPECT_TRUE(has(page, "PlayerBase"));
    EXPECT_TRUE(has(page, "OnUpdate"));
    EXPECT_TRUE(has(page, "M/scripts/4_World/x.c"));
}

// Ради этой секции отчёт и писался.
TEST(Html, OverrideChainIsRendered) {
    symbol_table syms;
    const auto a = put(syms, "TraderPlus", "PlayerBase", "OnUpdate");
    const auto b = put(syms, "$vanilla", "PlayerBase", "OnUpdate");
    call_tree tree;
    add(tree, {a}, 30);
    add(tree, {a, b}, 70);

    const auto page = to_html(tree, krprof::sample_log{}, syms, run_meta{100, 500, 1000, 0});
    EXPECT_TRUE(has(page, "Цепочки переопределений"));
    EXPECT_TRUE(has(page, "PlayerBase::OnUpdate"));
}

// Имена приходят из чужой памяти: угловая скобка в имени не должна ломать страницу.
TEST(Html, NamesAreEscaped) {
    symbol_table syms;
    const auto a = put(syms, "M", "array<int>", "f&g");
    call_tree tree;
    add(tree, {a}, 1);

    const auto page = to_html(tree, krprof::sample_log{}, syms, run_meta{1, 500, 1000, 0});
    EXPECT_TRUE(has(page, "array&lt;int&gt;"));
    EXPECT_TRUE(has(page, "f&amp;g"));
}

// Флеймграф отдаётся данными в страницу; если разметка данных поедет, график молча
// исчезнет — проверяем, что он вообще есть и что имена в него попали.
TEST(Html, FlameGraphDataIsEmbedded) {
    symbol_table syms;
    const auto a = put(syms, "KR_CORE", "JsonBuilder", "AddString");
    const auto b = put(syms, "KR_CORE", "JsonBuilder", "EscapeString");
    call_tree tree;
    add(tree, {a, b}, 40);

    const auto page = to_html(tree, krprof::sample_log{}, syms, run_meta{40, 1000, 1000, 0});
    EXPECT_TRUE(has(page, "Флеймграф"));
    EXPECT_TRUE(has(page, "const FG="));
    EXPECT_TRUE(has(page, "JsonBuilder::EscapeString"));
    EXPECT_TRUE(has(page, "id=\"fg\""));
}

// Кавычка в имени порвала бы JSON и оставила страницу без графика.
TEST(Html, FlameDataEscapesQuotes) {
    symbol_table syms;
    const auto a = put(syms, "M", "C", "say\"hi");
    call_tree tree;
    add(tree, {a}, 1);

    const auto page = to_html(tree, krprof::sample_log{}, syms, run_meta{1, 1000, 1000, 0});
    EXPECT_TRUE(has(page, "say\\\"hi"));
}
