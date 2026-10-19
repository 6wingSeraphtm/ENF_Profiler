#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "krprof/call_tree.hpp"

// Журнал снимков В ПОРЯДКЕ ВРЕМЕНИ.
//
// Дерево (`call_tree`) схлопывает одинаковые пути — в этом его смысл и его потеря:
// времени в нём нет. А игра однопоточна и идёт кадрами, поэтому вопрос «какой кадр был
// на 200 мс» требует именно последовательности. Две структуры, две задачи; сэмплер
// кладёт снимок в обе.
//
// Хранение плоское: все кадры подряд в одном векторе, на снимок — смещение и длина.
// Вектор векторов на 15 000 снимков это 15 000 аллокаций, и все на горячем пути.
namespace krprof {

class sample_log {
public:
    void add(std::span<const frame_id> stack, std::uint64_t at_us) {
        starts_.push_back(static_cast<std::uint32_t>(flat_.size()));
        lens_.push_back(static_cast<std::uint32_t>(stack.size()));
        times_.push_back(at_us);
        flat_.insert(flat_.end(), stack.begin(), stack.end());
    }

    [[nodiscard]] std::size_t size() const noexcept { return starts_.size(); }

    [[nodiscard]] std::span<const frame_id> stack_at(std::size_t i) const noexcept {
        return {flat_.data() + starts_[i], lens_[i]};
    }

    [[nodiscard]] std::uint64_t time_at(std::size_t i) const noexcept { return times_[i]; }

    void clear() noexcept {
        flat_.clear();
        starts_.clear();
        lens_.clear();
        times_.clear();
    }

private:
    std::vector<frame_id> flat_;
    std::vector<std::uint32_t> starts_;
    std::vector<std::uint32_t> lens_;
    std::vector<std::uint64_t> times_;
};

}  // namespace krprof
