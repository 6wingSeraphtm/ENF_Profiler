#include "krprof/analysis.hpp"

#include <algorithm>
#include <unordered_map>

namespace krprof {
namespace {

// Собственный вес каждого кадра: сумма по всем местам дерева, где он оказался листом.
std::unordered_map<frame_id, std::uint64_t> self_by_frame(const call_tree& tree) {
    std::unordered_map<frame_id, std::uint64_t> out;
    for (const folded_entry& e : tree.flatten()) {
        if (!e.path.empty()) { out[e.path.back()] += e.self; }
    }
    return out;
}

}  // namespace

std::vector<func_stat> by_function(const call_tree& tree, const symbol_table&) {
    std::vector<func_stat> out;
    for (const auto& [id, self] : self_by_frame(tree)) { out.push_back({id, self}); }

    // Дороже — выше; id вторым ключом, чтобы порядок не зависел от обхода хеш-таблицы.
    std::ranges::sort(out, [](const func_stat& a, const func_stat& b) {
        return a.self != b.self ? a.self > b.self : a.id < b.id;
    });
    return out;
}

std::vector<override_chain> override_chains(const call_tree& tree,
                                            const symbol_table& symbols) {
    // Ключ цепочки — `Класс::Метод`. Мод в ключ не входит: в этом весь смысл, мы как раз
    // и хотим свести под одно имя всех, кто его переопределил.
    std::unordered_map<std::string, override_chain> chains;

    for (const auto& [id, self] : self_by_frame(tree)) {
        const symbol& s = symbols[id];
        // У глобальной функции класса нет, переопределять нечего.
        if (s.klass.empty() || s.func.empty()) { continue; }

        const std::string key = s.klass + "::" + s.func;
        override_chain& c = chains[key];
        c.name = key;
        c.total += self;

        // Один мод может править метод из двух своих файлов — складываем, иначе строка
        // задвоится и проценты поедут.
        const auto it = std::ranges::find_if(c.links, [&](const override_link& l) {
            return l.mod == s.mod;
        });
        if (it != c.links.end()) {
            it->self += self;
        } else {
            c.links.push_back({s.mod, self, s.file, s.line});
        }
    }

    std::vector<override_chain> out;
    for (auto& [key, c] : chains) {
        // Один мод — это обычная функция, а не цепочка. Показывать её как
        // «переопределение» значило бы утопить настоящие цепочки в шуме.
        if (c.links.size() < 2) { continue; }
        std::ranges::sort(c.links, [](const override_link& a, const override_link& b) {
            return a.self != b.self ? a.self > b.self : a.mod < b.mod;
        });
        out.push_back(std::move(c));
    }

    std::ranges::sort(out, [](const override_chain& a, const override_chain& b) {
        return a.total != b.total ? a.total > b.total : a.name < b.name;
    });
    return out;
}

}  // namespace krprof

namespace krprof {

std::vector<flame_node> flame_tree(const call_tree& tree) {
    std::vector<flame_node> out;
    out.push_back(flame_node{0, 0, 0, 0, true, {}});

    // Пересобираем дерево из путей: у `call_tree` наружу торчат только они, и этого
    // достаточно — путь и есть ветка. Ключ ребра тот же, что там: (родитель, кадр).
    std::unordered_map<std::uint64_t, std::uint32_t> edge;
    for (const folded_entry& e : tree.flatten()) {
        std::uint32_t at = 0;
        for (const frame_id id : e.path) {
            const std::uint64_t key = (static_cast<std::uint64_t>(at) << 32) | id;
            const auto [it, fresh] = edge.try_emplace(key,
                                                      static_cast<std::uint32_t>(out.size()));
            if (fresh) {
                out.push_back(flame_node{id, 0, 0, out[at].depth + 1, false, {}});
                out[at].children.push_back(it->second);
            }
            at = it->second;
            // Вес листа принадлежит всем, кто его звал: так и получается ширина полоски.
            out[at].total += e.self;
        }
        out[0].total += e.self;
        if (at != 0) { out[at].self += e.self; }
    }

    for (flame_node& n : out) {
        std::ranges::sort(n.children, [&](std::uint32_t a, std::uint32_t b) {
            return out[a].total != out[b].total ? out[a].total > out[b].total
                                                : out[a].id < out[b].id;
        });
    }
    return out;
}

}  // namespace krprof

namespace krprof {

std::vector<episode> episodes(const sample_log& log,
                              const symbol_table&,
                              std::uint64_t period_us) {
    std::vector<episode> out;
    if (log.size() == 0) { return out; }
    const std::uint64_t step = period_us == 0 ? 1 : period_us;

    std::unordered_map<frame_id, std::uint64_t> leaves;
    std::size_t from = 0;

    const auto close = [&](std::size_t last) {
        episode e;
        e.start_us = log.time_at(from);
        // Последний снимок держится ещё один шаг: дольше мы про него не знаем.
        e.dur_us = log.time_at(last) - e.start_us + step;
        e.samples = last - from + 1;
        for (const auto& [id, n] : leaves) {
            if (n > e.top_samples) {
                e.top = id;
                e.top_samples = n;
            }
        }
        out.push_back(e);
        leaves.clear();
    };

    for (std::size_t i = 0; i < log.size(); ++i) {
        if (i != 0 && log.time_at(i) - log.time_at(i - 1) > step) {
            close(i - 1);
            from = i;
        }
        const auto stack = log.stack_at(i);
        if (!stack.empty()) { ++leaves[stack.back()]; }
    }
    close(log.size() - 1);

    std::ranges::sort(out, [](const episode& a, const episode& b) {
        return a.dur_us != b.dur_us ? a.dur_us > b.dur_us : a.start_us < b.start_us;
    });
    return out;
}

}  // namespace krprof
