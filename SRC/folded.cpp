#include "krprof/folded.hpp"

#include "krprof/attribution.hpp"

namespace krprof {
namespace {

// В формате `;` разделяет кадры, а пробел отделяет вес. Имена приходят из чужой памяти
// движка, и один такой символ внутри имени сломает файл МОЛЧА: строка разберётся, но не
// так, как задумано. Поэтому такие символы заменяются, а не выпалываются, — длина имени
// сохраняется, и подмена видна глазом.
void append_clean(std::string& out, std::string_view text) {
    for (const char c : text) {
        const bool breaks_format = c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
        out.push_back(breaks_format ? '_' : c);
    }
}

// Имя кадра: `мод|Класс::Функция`. У глобальной функции класса нет — и `::` тоже.
// Неразобранный кадр обязан остаться ВИДИМЫМ: дырка в флеймграфе врёт сильнее, чем
// честная строка «неизвестно».
void append_frame(std::string& out, const symbol& s) {
    append_clean(out, s.mod.empty() ? unknown_mod : std::string_view{s.mod});
    out.push_back('|');

    if (!s.klass.empty()) {
        append_clean(out, s.klass);
        out.append("::");
    }
    if (s.func.empty()) {
        out.push_back('?');
    } else {
        append_clean(out, s.func);
    }
}

}  // namespace

std::string to_folded(const call_tree& tree, const symbol_table& symbols) {
    std::string out;

    for (const folded_entry& entry : tree.flatten()) {
        bool first = true;
        for (const frame_id id : entry.path) {
            if (!first) { out.push_back(';'); }
            first = false;
            append_frame(out, symbols[id]);
        }
        out.push_back(' ');
        out.append(std::to_string(entry.self));
        out.push_back('\n');
    }
    return out;
}

}  // namespace krprof
