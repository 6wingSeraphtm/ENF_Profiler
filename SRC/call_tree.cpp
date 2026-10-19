#include "krprof/call_tree.hpp"

#include <algorithm>

namespace krprof {

void call_tree::add(std::span<const frame_id> stack, std::uint64_t weight) {
    // Пустой стек — штатный исход: сэмплер ловит поток и тогда, когда скрипт не
    // исполняется вовсе. Нулевой вес класть тоже некуда.
    if (stack.empty() || weight == 0) { return; }

    std::uint32_t parent = no_parent;
    for (const frame_id id : stack) {
        const auto [it, fresh] = edges_.try_emplace(edge_key(parent, id),
                                                    static_cast<std::uint32_t>(nodes_.size()));
        if (fresh) { nodes_.push_back(node{id, parent, 0}); }
        parent = it->second;
    }

    // Вес достаётся ТОЛЬКО последнему кадру: он и был на вершине в момент снимка.
    // Предки в этот миг ничего не исполняли — они ждали, и их собственное время
    // набирается из тех снимков, где на вершине были они сами.
    nodes_[parent].self += weight;
    total_ += weight;
}

void call_tree::clear() noexcept {
    nodes_.clear();
    edges_.clear();
    total_ = 0;
}

std::vector<folded_entry> call_tree::flatten() const {
    std::vector<folded_entry> out;

    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].self == 0) { continue; }

        // Путь собирается от листа вверх по родителям — вниз ссылок нет и не нужно.
        folded_entry entry;
        entry.self = nodes_[i].self;
        for (std::uint32_t at = static_cast<std::uint32_t>(i); at != no_parent;
             at = nodes_[at].parent) {
            entry.path.push_back(nodes_[at].id);
        }
        std::ranges::reverse(entry.path);
        out.push_back(std::move(entry));
    }

    std::ranges::sort(out, [](const folded_entry& a, const folded_entry& b) {
        return a.path < b.path;
    });
    return out;
}

}  // namespace krprof
