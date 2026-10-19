"""Раскопки по бинарю игры БЕЗ IDA: строки, ссылки на них, дизассемблер вокруг.

Зачем это есть. Раскладку `DebugInfo` тремя заходами не удалось нащупать поиском по
памяти в рантайме — признаки «массив путей» и «таблица строк» ловят чужие структуры.
Значит смотреть надо не в память, а в КОД: движок сам печатает `@"файл.c,строка"`, то
есть внутри него лежит готовый обход этой самой структуры. Из инструкций смещения
читаются однозначно, а не угадываются.

    python dig.py <exe> str   '@"%s,%d"'          где лежит строка
    python dig.py <exe> refs  0x140e2eaf8         кто на неё ссылается (lea/mov rip-rel)
    python dig.py <exe> dis   0x140352DB0 0x200   дизассемблировать
    python dig.py <exe> back  0x140352DB0         найти начало функции и показать её
    python dig.py <exe> xref  0x140352DB0         прямые call/jmp на адрес
"""
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<Q", d, opt + 24)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        so = opt + struct.unpack_from("<H", d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = so + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vs, va, rs, ptr = struct.unpack_from("<IIII", d, o + 8)
            self.secs.append((name, self.base + va, vs, ptr, rs))
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True

    def off(self, va):
        for _, sva, vs, ptr, rs in self.secs:
            if sva <= va < sva + vs:
                o = ptr + (va - sva)
                return o if o < len(self.d) else None
        return None

    def read(self, va, n):
        o = self.off(va)
        return self.d[o:o + n] if o is not None else b""

    def text(self):
        for name, sva, vs, ptr, rs in self.secs:
            if name == ".text":
                return sva, vs, ptr, rs
        return None

    def find_str(self, s):
        """Все адреса, где лежит эта строка (как байты, с нулём на конце)."""
        want = s.encode() + b"\0"
        out, at = [], 0
        while True:
            at = self.d.find(want, at)
            if at < 0:
                return out
            for _, sva, vs, ptr, rs in self.secs:
                if ptr <= at < ptr + rs:
                    out.append(sva + (at - ptr))
                    break
            at += 1

    def refs(self, target):
        """Инструкции, адресующие target через rip-относительный операнд.

        Именно так x64 обращается к строкам и глобалям: `lea rcx,[rip+disp]`. Ищем
        линейным проходом по .text — попыток синхронизации не делаем, потому что нам
        нужны не все инструкции, а только совпавшие по вычисленному адресу.
        """
        sva, vs, ptr, rs = self.text()
        code = self.d[ptr:ptr + rs]
        out = []
        # Идём с каждого смещения: дизассемблер сам отбросит мусор, а совпадение по
        # адресу настолько избирательно, что ложных срабатываний практически нет.
        for start in range(0, len(code) - 16):
            for ins in self.md.disasm(code[start:start + 16], sva + start):
                for op in ins.operands:
                    if op.type == 3 and op.mem.base == 41:  # x86_OP_MEM, RIP
                        if ins.address + ins.size + op.mem.disp == target:
                            out.append((ins.address, f"{ins.mnemonic} {ins.op_str}"))
                break
        return out

    def fastrefs(self, target):
        """То же, что refs, но одним линейным проходом и без дизассемблера.

        У rip-относительного операнда смещение считается от КОНЦА инструкции, поэтому
        для каждой позиции p достаточно проверить: read_i32(p) + va(p) + 4 == target.
        Дизассемблировать при этом нечего — а полный проход capstone'ом по 16 МБ .text
        занимал минуты.
        """
        sva, vs, ptr, rs = self.text()
        code = self.d[ptr:ptr + rs]
        out = []
        for p in range(0, len(code) - 4):
            disp = struct.unpack_from("<i", code, p)[0]
            if disp + sva + p + 4 == target:
                out.append(sva + p)
        return out

    def dis(self, va, n=0x100):
        code = self.read(va, n)
        return "\n".join(
            f"0x{i.address:x}  {i.mnemonic:<8} {i.op_str}" for i in self.md.disasm(code, va)
        )

    def func_start(self, va, back=0x800):
        """Грубый поиск начала функции: ближайший назад пролог или int3-выравнивание."""
        code = self.read(va - back, back)
        if not code:
            return None
        best = None
        for i in self.md.disasm(code, va - back):
            if i.mnemonic == "int3":
                best = i.address + i.size
        return best

    def xrefs(self, target):
        sva, vs, ptr, rs = self.text()
        code = self.d[ptr:ptr + rs]
        out = []
        for start in range(0, len(code) - 16):
            for ins in self.md.disasm(code[start:start + 16], sva + start):
                if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
                    if int(ins.op_str, 16) == target:
                        out.append(ins.address)
                break
        return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    p = PE(argv[1])
    cmd = argv[2]
    if cmd == "str":
        for a in p.find_str(argv[3]):
            print(hex(a))
    elif cmd == "fastrefs":
        for a in p.fastrefs(int(argv[3], 0)):
            print(f"0x{a:x}  (поле смещения; инструкция начинается на 3-4 байта раньше)")
    elif cmd == "refs":
        for a, txt in p.refs(int(argv[3], 0)):
            print(f"0x{a:x}  {txt}")
    elif cmd == "dis":
        n = int(argv[4], 0) if len(argv) > 4 else 0x100
        print(p.dis(int(argv[3], 0), n))
    elif cmd == "back":
        s = p.func_start(int(argv[3], 0))
        print("начало функции:", hex(s) if s else "не найдено")
        if s:
            print(p.dis(s, int(argv[3], 0) - s + 0x80))
    elif cmd == "xref":
        for a in p.xrefs(int(argv[3], 0)):
            print(hex(a))
    else:
        print("неизвестная команда", cmd)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
