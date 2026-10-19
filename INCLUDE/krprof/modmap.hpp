#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "krprof/attribution.hpp"

// Схлопывание подкаталогов мода в сам мод.
//
// `mod_of` смотрит на ОДИН путь и потому не может отличить подкаталог мода от его
// префикса: у `KR/core/KR_CORE/MATH/2_GameLib/...` перед слоем стоит `MATH`, и по одному
// пути это неотличимо от настоящего мода. На живом сервере это дало четыре «мода» —
// MATH, CORO, EVENT, UI — там, где мод один, KR_CORE.
//
// Различие видно только по НАБОРУ путей: если несколько разных имён сидят под общим
// родителем, значит родитель и есть мод, а они — его части. Поэтому правило работает над
// множеством и живёт отдельно от `mod_of`, оставляя ту чистой функцией.
namespace krprof {

class mod_map {
public:
    // Показать карте ещё один путь. Зовётся на символизации каждого нового кадра.
    void observe(std::string_view path);

    // Досчитать: решить, какие имена схлопнуть в родителя. После этого `of` отвечает.
    void finish();

    // Имя мода для пути. До `finish()` и для незнакомых путей — обычное правило.
    [[nodiscard]] std::string_view of(std::string_view path) const;

private:
    // Полный префикс -> сколько разных коротких имён под его РОДИТЕЛЕМ.
    std::unordered_map<std::string, std::unordered_set<std::string>> children_;
    // Короткое имя -> чем его заменить.
    std::unordered_map<std::string, std::string> collapse_;
};

}  // namespace krprof
