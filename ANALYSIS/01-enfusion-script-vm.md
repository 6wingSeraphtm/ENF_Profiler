# Enfusion: скриптовая машина глазами профайлера

Источники:

- `E:\source\DayZ-0.45.124252\Enfusion` — исходники движка (старая ветка, 0.45).
  **Это карта, а не территория:** раскладка структур там своя, retail-бинарь ушёл далеко
  вперёд. Годится, чтобы понять МЕХАНИЗМ и имена полей; смещения берутся только зондом.
- `P:\scripts` — ванильные скрипты текущей игры (`1_Core/proto/EnProfiler.c`).
- `E:\source\KR_GRAFTED_PTY` — graft: то, что уже умеет находить эти структуры вживую.

---

## 1. Как движок зовёт скриптовую функцию

Кадр вызова кладётся в **скриптовый поток**, а не на стек C++.

```
ScriptThread {
    ...
    uint    m_iStackPos;               // индекс текущего кадра
    VMStack m_sStack[VMSTACK_SIZE];    // массив кадров — ЭТО И ЕСТЬ СТЕК ВЫЗОВОВ
}

VMStack {
    const Stat*   m_psStat;       // текущая инструкция
    const Funct*  m_pfFunc;       // ЧТО исполняется
    Instance*     m_peInstance;   // НА ЧЁМ исполняется (this)
    int           m_iStored, m_iStoredOff;
#ifdef ENF_DEVELOPER
    TimeStamp     m_Time;         // отметка входа — только у dev-сборки
#endif
    void*         m_pStackPointer;
}
```

`enf_scripttype.h`, `enf_scriptthread.h`.

### Вложенный вызов НЕ рекурсирует в C++

`enf_scriptvm.cpp`, `OP_CALL`:

```cpp
case OP_CALL:
    stack = PushCall(ref.Module, currentThread, ref.pInstance, NULL, ref.FunctionIndex);
    ...
    // ПРОДОЛЖАЕТСЯ ТОТ ЖЕ ЦИКЛ ИНТЕРПРЕТАТОРА
case OP_RTS:
    stack = PopCall(currentThread);
```

**Следствие для профайлера.** Границы «вход в функцию / выход» как вызова C++ не
существует: снаружи виден один `interp(...)` на всю глубину скрипта. Значит:

- детур на «вход в функцию» повесить **не на что** — кроме самих `PushCall`/`PopCall`,
  если они в retail остались отдельными функциями (проверяется, см. `03-`);
- зато **весь стек лежит в памяти сплошным массивом** и читается в любой момент.
  Это ровно то, что нужно сэмплирующему профайлеру.

---

## 2. Ванильный профайлер: что он делает и чего не может

`enf_scriptthread.cpp` — весь механизм, три места:

```cpp
// PushCall / IPushCall
if (m_bProfile) { stack->m_Time = Timer::GetTicks(); fn->Profile++; }

// PopCall
if (m_bProfile) {
    TimeStamp dtime = Timer::Sub(Timer::GetTicks(), stack->m_Time);
    if (stack->m_peInstance) stack->m_peInstance->m_pcClass->Time += dtime;
    stack->m_pfFunc->Time += dtime;
}
```

Счётчики живут **на самих `Funct` и `Class`** (`Profile` — счёт вызовов, `Time` — сумма).
`ScriptProfile` (`enf_scriptprofile.cpp`) потом просто обходит все функции и классы модуля
и сортирует три плоских списка: по времени класса, по счёту вызовов, по времени функции.

### Ограничения — все принципиальные

| Что | Почему так |
|---|---|
| **Нет дерева вызовов** | счётчик на `Funct`, а не на пути. Флеймграф из этого не строится в принципе |
| **Время инклюзивное** | `fn->Time` включает всех детей; self-time не считается нигде |
| **Рекурсия двоится** | каждый уровень прибавляет весь свой поддиапазон в тот же `Funct` |
| **`proto` не считаются** | подтверждено доками: *"'proto' methods are not tracked, but will contribute to a script methods total time"* |
| **Атрибуция — по модулям, не по модам** | `EnProfilerModule` = {CORE, GAMELIB, GAME, WORLD, MISSION}. Какой МОД дал override — не знает |
| **Только dev/diag сборка** | `#ifdef ENF_DEVELOPER` + доки: *"Only available on developer and diag builds"*. На боевом сервере API нет |
| **Override не отделён от базы** | `FromSuper()` в `enf_scriptprofile.cpp` наследованные записи просто **выбрасывает** |

Последняя строка — самая важная для нас. Вот этот код:

```cpp
if (!FromSuper(cl, cl->GetMethod(f), f))       // <- унаследованное молча пропускается
    prof.AddFunction(ScriptProfile::Item(cl, cl->GetMethod(f)));
```

То есть «стоимость конкретного override'а конкретным модом» ванильный профайлер не то что
не показывает — он этот случай **специально отфильтровывает**.

**Вывод: строить поверх EnProfiler нечего.** Он недоступен на боевом сервере, плоский по
устройству и слеп к модам. Нужен свой сбор.

---

## 3. Откуда берётся имя мода — цепочка атрибуции

Ключ ко всей задаче. `Funct` несёт `DebugOffset`:

```cpp
class Funct {
    ScriptModule* Module;       // 1_Core / 3_Game / ... — не мод
    const char*   Name;         // имя функции
    uint          DebugOffset;  // индекс в DebugInfo::m_Lines
}
```

`enf_scriptdebuginfo.h`:

```cpp
class DebugInfo {
    struct Module { DString m_strName; };            // ИМЯ ИСХОДНОГО ФАЙЛА
    struct Line   { uint m_iOffset; ushort m_iSourceLine; ushort m_iModule; };
    Array<Module> m_Modules;
    Array<Line>   m_Lines;
}
```

А `m_strName` — это то, что компилятор получил от парсера (`enf_scriptcompiler.cpp:95`):

```cpp
m_pModule->m_pDebugInfo->AddDebugLine(
    m_pParser->GetModule(m_pDLine->m_iModule)->GetName(),   // путь исходника
    m_pDLine->m_iSourceLine, offset);
```

**Итого цепочка:**

```
Funct*  ->  DebugOffset  ->  m_Lines[off].m_iModule  ->  m_Modules[i].m_strName
        ->  путь к .c файлу  ->  мод
```

Это прямая атрибуция «функция → файл → мод», и она не требует ни обхода иерархии, ни
догадок. Живость подтверждается косвенно, но надёжно: **ошибки скрипта в DayZ печатают
файл и строку на боевом сервере** — значит `DebugInfo` в рантайме есть.

Объявляющий класс (для строки «кто именно переопределил») берётся отдельно — обходом
цепочки `Super` вверх, пока `methods[idx]` остаётся тем же указателем `Funct*`. Самый
верхний класс, у которого он ещё тот же, и есть объявляющий. Но для атрибуции **мода**
достаточно файла, и это дешевле.

---

## 4. Что из этого уже умеет graft

`graft::mt` (`MT/src/machine.cpp`) находит вживую, **без единого зашитого адреса**, идя от
якоря `ScriptModule.Call`:

| Найдено | Зачем профайлеру |
|---|---|
| `interp`, `invoke_method`, `create_thread` | точки входа скриптовой машины |
| `ctx_slot`, `ctx.current` | **текущий скриптовый поток** — с него начинается стек |
| `ctx.lock` | движковая критическая секция (ворота) |
| `thread.frames`, `frame_size`, `frame_count` | **массив кадров = стек вызовов** |
| `thread.frame_index` | **вершина стека** |
| `thread.state`, `values`, `stack_top` | состояние потока |
| `detail::game_thread_id()` | поток игрового цикла — кого сэмплировать |

`graft` (основной) знает раскладку объекта и класса: `object_class = 8`,
`class_name = 16`, `class_functions = 104`, `desc_impl = 8`, `desc_flags = 80`.

**Это примерно половина того, что нужно.** Стек мы уже умеем найти и обойти.

### Чего НЕ хватает — список на добор зондом

1. Раскладка кадра (`call_frame`): смещения `m_pfFunc` и `m_peInstance` внутри кадра.
   По исходнику это 8 и 16, но `frame_size` в retail ищется динамически — сверить.
2. Раскладка `Funct` в retail: `Name`, `Module`, `DebugOffset`.
   `graft::layout` уже знает `desc_impl`/`desc_flags`/`desc_params` — остальные добираются
   тем же способом.
3. `DebugInfo`: как достать его от `ScriptModule`, раскладка `m_Modules` / `m_Lines`.
4. Есть ли в retail `ENF_DEVELOPER` (влияет на `frame_size`: с `m_Time` кадр на 8 байт
   длиннее). Ответ даёт сам найденный `frame_size`.
5. Остались ли `PushCall`/`PopCall` отдельными функциями (не заинлайнены) — от этого
   зависит, возможна ли точная трассировка вдобавок к сэмплированию.

Методика добора — та же, что у graft: зонд на живом сервере (скрипт кладёт известное
значение, C++ ищет его в памяти) плюс байтовая сигнатура, и всё закрепляется тестом.
