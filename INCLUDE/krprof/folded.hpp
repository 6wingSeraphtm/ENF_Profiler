#pragma once
#include <string>

#include "krprof/call_tree.hpp"
#include "krprof/symbols.hpp"

// Свёрнутые стеки — файл для готового просмотрщика.
//
//   мод|Класс::Функция;мод|Класс::Функция <вес>
//
// Одна строка на путь, вес — СОБСТВЕННЫЙ вес этого пути; предков просмотрщик складывает
// сам. Формат читают speedscope, inferno и flamegraph.pl без конвертации.
//
// Почему атрибуция живёт прямо в имени кадра: так она видна в ЛЮБОМ просмотрщике, и
// дорабатывать просмотрщик не надо. Цепочка override'ов при этом читается сама собой —
// `super.OnUpdate()` кладёт новый кадр с `Funct` базы, поэтому мод каждого звена стоит
// на своём кадре, а стоимость звена — это его собственный вес.
namespace krprof {

[[nodiscard]] std::string to_folded(const call_tree& tree, const symbol_table& symbols);

}  // namespace krprof
