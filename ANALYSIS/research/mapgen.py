"""Карта методов движка по исходникам: «бедный PDB».

ИДЕЯ. У DayZDiag нет PDB и всего три экспорта, поэтому имён движковых функций взять
неоткуда. Но есть исходники Enfusion 0.45 — старые, разошедшиеся с retail, и всё же
несущие то, что переживает годы правок: СТРОКОВЫЕ ЛИТЕРАЛЫ. Сообщение об ошибке, формат
лога, текст assert'а компилятор кладёт в образ дословно.

Значит связь строится так:

    строка в исходнике -> функция, где она лежит        (разбор исходников)
    та же строка в образе -> кто на неё ссылается        (скан ссылок)
    ссылка -> начало функции                            (таблица .pdata)
    => RVA функции -> её имя

Метод не гипотетический: именно так вручную был найден `ScriptContext::GetCallStack`
(по строке `"\\nStack trace:"`), и от него разобрана вся отладочная информация.

    python mapgen.py <exe> <каталог Enfusion> [выход.symbols]

Осторожно с доверием: совпадение строки — довод, а не доказательство. Скрипт отдаёт
только ОДНОЗНАЧНЫЕ случаи (строка встречается в одной функции исходника и на неё
ссылается одна функция образа) и помечает остальные как спорные.
"""
import collections
import re
import struct
import sys
from pathlib import Path

# Строки короче этого не берём: «ok», «%s» и подобное встречается всюду и ничего не
# опознаёт, только плодит ложные связи.
MIN_LEN = 10

# Литерал в C++. Экранированные кавычки внутри учитываем, многострочную склейку — нет:
# она редка, а ошибиться на ней дороже, чем пропустить.
STR_RE = re.compile(r'"((?:[^"\\\n]|\\.){%d,})"' % MIN_LEN)

# Определение функции: `Тип Класс::Имя(` в начале строки. Намеренно грубо — нам нужно
# имя, а не разбор C++.
FUNC_RE = re.compile(r'^[A-Za-z_][\w\s:*&<>,]*?\b(\w+::~?\w+)\s*\(')


def unescape(s):
    """Литерал -> байты, как их положит компилятор."""
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c != '\\':
            out.extend(c.encode('utf-8', 'ignore'))
            i += 1
            continue
        i += 1
        if i >= len(s):
            break
        e = s[i]
        i += 1
        mapping = {'n': 10, 't': 9, 'r': 13, '0': 0, '\\': 92, '"': 34, "'": 39}
        if e in mapping:
            out.append(mapping[e])
        elif e == 'x':
            h = ''
            while i < len(s) and s[i] in '0123456789abcdefABCDEF':
                h += s[i]
                i += 1
            if h:
                out.append(int(h, 16) & 0xFF)
        else:
            out.extend(e.encode('utf-8', 'ignore'))
    return bytes(out)


def scan_sources(root):
    """Строка -> множество функций, в которых она встречается."""
    by_string = collections.defaultdict(set)
    files = list(Path(root).rglob('*.cpp')) + list(Path(root).rglob('*.h'))
    for path in files:
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        # Строка засчитывается ТОЛЬКО внутри тела функции.
        #
        # Две неудачные попытки до этого, обе поучительные:
        #   * счёт глубины от начала файла — `namespace enf {` уводит её в единицу
        #     навсегда, распознавание отключалось после первой строки (нашлось три);
        #   * «последняя виденная функция» — строки в статических таблицах и в функциях,
        #     которые регулярка не узнала, приписывались предыдущей. `Engine::UpdateFrame`
        #     оказался на семи разных адресах, то есть имя было просто неверным.
        #
        # Теперь глубина считается ОТ ТЕЛА: увидели сигнатуру, ждём `{`, считаем до
        # возврата в ноль. За телом функции — ничьё.
        current = None
        depth = 0
        started = False
        for line in text.splitlines():
            if current is None:
                m = FUNC_RE.match(line)
                if m:
                    current = m.group(1)
                    depth = 0
                    started = False
                    # сигнатура и тело бывают на одной строке
                else:
                    continue

            opens = line.count('{')
            closes = line.count('}')
            if opens:
                started = True
            depth += opens - closes

            if started:
                for lit in STR_RE.findall(line):
                    raw = unescape(lit)
                    if len(raw) >= MIN_LEN:
                        by_string[raw].add(current)

            if started and depth <= 0:
                current = None
    return by_string, len(files)


class PE:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', self.d, 0x3C)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<Q', self.d, opt + 24)[0]
        nsec = struct.unpack_from('<H', self.d, pe + 6)[0]
        so = opt + struct.unpack_from('<H', self.d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = so + 40 * i
            name = self.d[o:o + 8].rstrip(b'\0').decode(errors='replace')
            vs, va, rs, ptr = struct.unpack_from('<IIII', self.d, o + 8)
            self.secs.append((name, va, vs, ptr, rs))
        magic = struct.unpack_from('<H', self.d, opt)[0]
        ddir = opt + (112 if magic == 0x20B else 96)
        self.pdata_rva, self.pdata_size = struct.unpack_from('<II', self.d, ddir + 8 * 3)

    def off(self, rva):
        for _, va, vs, ptr, _ in self.secs:
            if va <= rva < va + vs:
                return ptr + (rva - va)
        return None

    def text(self):
        for name, va, vs, ptr, rs in self.secs:
            if name == '.text':
                return va, vs, ptr, rs
        raise RuntimeError('нет .text')

    def functions(self):
        """Начала функций из таблицы раскрутки — точные границы без всяких символов."""
        o = self.off(self.pdata_rva)
        out = []
        for i in range(self.pdata_size // 12):
            begin, end, _ = struct.unpack_from('<III', self.d, o + 12 * i)
            if begin:
                out.append((begin, end))
        out.sort()
        return out

    def find_bytes(self, needle):
        """RVA всех вхождений (строка ищется как есть, с нулём на конце)."""
        want = needle + b'\0'
        out, at = [], 0
        while True:
            at = self.d.find(want, at)
            if at < 0:
                return out
            for _, va, vs, ptr, rs in self.secs:
                if ptr <= at < ptr + rs:
                    out.append(va + (at - ptr))
                    break
            at += 1

    def refs_to(self, rvas):
        """RVA инструкций, адресующих любой из rvas через rip-относительный операнд.

        Один линейный проход по .text на все строки сразу: перебирать образ отдельно для
        каждой из тысяч строк заняло бы часы.
        """
        want = set(rvas)
        va, vs, ptr, rs = self.text()
        code = self.d[ptr:ptr + rs]
        hits = collections.defaultdict(list)
        for p in range(len(code) - 4):
            disp = struct.unpack_from('<i', code, p)[0]
            target = disp + va + p + 4
            if target in want:
                hits[target].append(va + p)
        return hits


def enclosing(funcs, rva):
    """Функция, которой принадлежит адрес: двоичный поиск по таблице раскрутки."""
    lo, hi = 0, len(funcs)
    while lo < hi:
        mid = (lo + hi) // 2
        if funcs[mid][0] <= rva:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None
    begin, end = funcs[lo - 1]
    return begin if begin <= rva < end else None


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    exe, src = argv[1], argv[2]
    out_path = argv[3] if len(argv) > 3 else 'krprof.symbols'

    print('разбираю исходники...')
    by_string, nfiles = scan_sources(src)
    print(f'  файлов {nfiles}, строк-кандидатов {len(by_string)}')

    print('ищу их в образе...')
    pe = PE(exe)
    located = {}
    for raw, funcs in by_string.items():
        rvas = pe.find_bytes(raw)
        # Не требуем единственного вхождения: строка часто лежит и в коде, и в таблице
        # ресурсов. Голосование ниже само отбросит противоречивые случаи, а лишний
        # кандидат стоит дёшево.
        if 1 <= len(rvas) <= 3:
            for r in rvas:
                located[r] = (raw, funcs)
    print(f'  найдено в образе однозначно: {len(located)}')

    print('ищу ссылки на них (один проход по .text)...')
    hits = pe.refs_to(located.keys())
    print(f'  строк со ссылками: {len(hits)}')

    funcs = pe.functions()
    print(f'  функций в таблице раскрутки: {len(funcs)}')

    # Кандидаты: начало функции -> какие имена ей приписывают строки.
    votes = collections.defaultdict(collections.Counter)
    for srva, sites in hits.items():
        raw, names = located[srva]
        for site in sites:
            fn = enclosing(funcs, site)
            if fn is None:
                continue
            for n in names:
                votes[fn][n] += 1

    # Имя обязано принадлежать ОДНОЙ функции. Если строки одного имени привели к
    # нескольким началам функций, значит привязка в исходниках неверна — выбрасываем всё
    # это имя целиком, а не выбираем из него наугад.
    per_name = collections.Counter()
    for fn, counter in votes.items():
        for n in counter:
            per_name[n] += 1
    ambiguous = {n for n, k in per_name.items() if k > 1}

    sure, disputed = {}, {}
    for fn, counter in votes.items():
        counter = collections.Counter({n: c for n, c in counter.items()
                                       if n not in ambiguous})
        if not counter:
            continue
        if len(counter) == 1:
            sure[fn] = next(iter(counter))
        else:
            top = counter.most_common()
            # Явный перевес — принимаем; иначе спорно.
            if top[0][1] >= 2 * top[1][1]:
                sure[fn] = top[0][0]
            else:
                disputed[fn] = top

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('# RVA<TAB>имя  — карта методов движка по строкам из исходников 0.45\n')
        for fn in sorted(sure):
            f.write(f'{fn:#x}\t{sure[fn]}\n')

    print()
    print(f'ОДНОЗНАЧНО ОПОЗНАНО: {len(sure)} функций -> {out_path}')
    print(f'спорных: {len(disputed)}')
    print(f'покрытие таблицы раскрутки: {100.0 * len(sure) / max(len(funcs), 1):.2f}%')
    print('\nпримеры:')
    for fn in sorted(sure)[:15]:
        print(f'  {fn:#010x}  {sure[fn]}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
