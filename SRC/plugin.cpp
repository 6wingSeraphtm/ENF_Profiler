// KRPROF — профайлер скриптов. Плагин graft, живущий БЕЗ мода.
//
// Мода нет вовсе: ни PBO, ни объявлений нативов, ни строки в скрипте. Отсюда всё
// остальное устройство:
//
//   * DLL кладётся в `<игра>/grafted/` — загрузчик graft смотрит и туда,
//     не только в `@МОД/grafted/` (SRC/graft/loader.cpp);
//   * `GRAFT_ON_TICK` недоступен: тик приходит через натив `GraftTick`, который мод
//     дёргает из своего `OnUpdate`. Дёргать некому;
//   * `graft::stage` тоже недоступен: он линкуется в хост, а плагин линкует клиента.
//     Подписаться на «движок готов» нечем — приходится опрашивать самим;
//   * поэтому у профайлера СВОЙ поток. Он и так обязателен: сэмплирующий профайлер не
//     может снимать стек с того же потока, который снимает.
//
// ГДЕ ЗАПУСКАЕТСЯ ПОТОК. Не в статическом инициализаторе: те исполняются внутри
// `LoadLibrary`, то есть под loader lock, а создание потока оттуда — классический
// висяк. `GRAFT_BINDINGS` зовётся из `graft_plugin_entry`, уже после возврата из
// загрузчика, — вот там и можно.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <format>
#include <string>
#include <thread>

#include <graft/mt.hpp>
#include <graft/native.hpp>

#include "krprof/engine.hpp"
#include "krprof/sampler.hpp"

GRAFT_PLUGIN("KRPROF", 1);

namespace {

std::atomic<bool> g_started{false};

// Настройки съёма. Скриптового API у нас нет (мода нет вовсе), поэтому единственный
// способ управлять профайлером снаружи — файл рядом с exe игры: `krprof.cfg`,
// по строке `ключ=значение`. Нет файла — значения по умолчанию.
//
//   hz=500        частота съёма
//   delay_s=10    сколько ждать после подъёма движка (сервер на старте грузит мир)
//   seconds=30    сколько снимать
struct settings {
    unsigned hz = 500;       // замерено: окно ~2 мкс -> около 0.13 % времени сервера
    unsigned delay_s = 10;
    unsigned seconds = 30;
};

settings read_settings() {
    settings s;
    std::ifstream in{"krprof.cfg"};
    if (!in) { return s; }

    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || line.starts_with("#")) { continue; }
        const std::string key = line.substr(0, eq);
        // Ноль — законное значение (`delay_s=0` значит «снимать сразу»), поэтому
        // отличать «не задано» от нуля надо по РАЗБОРУ, а не по величине. Проверка
        // `value == 0` стоила прогона: задержка молча оставалась десятисекундной, и
        // нагрузка успевала отработать до начала съёма.
        const char* from = line.c_str() + eq + 1;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(from, &end, 10);
        if (end == from) { continue; }
        const unsigned value = static_cast<unsigned>(parsed);
        if (key == "hz") { s.hz = value; }
        else if (key == "delay_s") { s.delay_s = value; }
        else if (key == "seconds") { s.seconds = value; }
    }
    return s;
}

void say(std::string_view line) { graft::log(std::format("[KRPROF] {}", line)); }

// Ждём, пока движок поднимет скрипты. Сигнала об этом у плагина нет (см. шапку), поэтому
// опрашиваем — дёшево и с потолком: в процессе без движка (генератор объявлений тоже
// грузит эту DLL) поток просто уйдёт, а не останется висеть.
bool wait_for_engine() {
    constexpr int k_tries = 600;  // 600 x 100 мс = минута
    for (int i = 0; i < k_tries; ++i) {
        if (graft::mt::available()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void profiler_main() {
    if (!wait_for_engine()) {
        say(std::format("движок не поднялся: {}", graft::mt::unavailable_reason()));
        return;
    }

    const settings cfg = read_settings();
    say(std::format("движок найден; настройки: hz={} delay_s={} seconds={}",
                    cfg.hz, cfg.delay_s, cfg.seconds));

    // Сервер на старте занят загрузкой мира — скрипт там идёт рывками. Даём ему
    // устояться, иначе первые секунды профиля описывают загрузку, а не игру.
    std::this_thread::sleep_for(std::chrono::seconds(cfg.delay_s));
    krprof::sampler::run(cfg.hz, cfg.seconds, say);
}

}  // namespace

// Нативов не регистрируем ни одного — их некуда объявлять без PBO. Тело нужно ради
// самого вызова: это единственная точка плагина, которая исполняется вне loader lock.
GRAFT_BINDINGS("3_Game") {
    (void)bind;
    if (g_started.exchange(true)) { return; }
    std::thread{profiler_main}.detach();
}
