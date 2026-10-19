// Во что обходится сэмплирование тому, кого сэмплируют.
//
// ЗАЧЕМ ЭТО ЕСТЬ. Требование к профайлеру — не ронять FPS сервера, а сервер DayZ это один
// поток, идущий покадрово. Сэмплирующий профайлер обязан этот поток ОСТАНАВЛИВАТЬ: читать
// массив кадров скриптового потока, пока он же его перезаписывает, значит поймать рваной
// снимок и мусорный указатель. Значит цена профайлера — это цена окна остановки, и её
// надо знать числом, а не надеждой.
//
// ЧТО МЕРЯЕТСЯ. Поток-жертва крутит счётчик (модель игрового цикла), сэмплер с заданной
// частотой делает ровно то же, что будет делать настоящий: Suspend, GetThreadContext,
// снять кадры, Resume. Считаем, на сколько просела пропускная способность жертвы.
//
// Игра для этого не нужна: окно остановки не зависит от того, что именно в потоке.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

// Столько кадров снимаем за одну остановку. У движка предел глубины свой, но реальные
// скриптовые стеки DayZ — десятки кадров; берём с запасом.
constexpr int k_frames = 64;
// Кадр движка — три указателя плюс служебное; копируем представительный объём.
constexpr int k_frame_bytes = 40;

std::atomic<std::uint64_t> g_work{0};
std::atomic<bool> g_stop{false};

// Модель игрового цикла: считает и пишет в память, как и настоящий.
void victim() {
    std::uint64_t local = 0;
    std::vector<std::uint64_t> scratch(1024, 1);
    while (!g_stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 1024; ++i) { local += scratch[static_cast<std::size_t>(i)] * 3u; }
        scratch[local % 1024] = local;
        g_work.fetch_add(1, std::memory_order_relaxed);
    }
}

// Ровно то, что будет делать настоящий сэмплер, и ничего сверх: символизация и укладка в
// дерево идут ПОСЛЕ возобновления и в окно остановки не попадают.
std::uint64_t sample_once(HANDLE thread, std::vector<std::byte>& sink) {
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) { return 0; }

    // Контекст нужен по-настоящему: без него не узнать, где поток стоит.
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    GetThreadContext(thread, &ctx);

    // Снимок кадров — единственное, ради чего останавливали. Копируем и уходим.
    std::memcpy(sink.data(), &ctx, sizeof(void*) * 2);
    volatile std::byte sponge = sink[0];
    (void)sponge;

    ResumeThread(thread);
    return 1;
}

struct result {
    std::uint64_t work = 0;
    std::uint64_t samples = 0;
};

result run(int hz, int seconds) {
    g_work.store(0);
    g_stop.store(false);

    std::thread worker{victim};
    const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                     FALSE,
                                     GetThreadId(worker.native_handle()));

    std::vector<std::byte> sink(k_frames * k_frame_bytes);
    std::uint64_t samples = 0;

    const auto started = std::chrono::steady_clock::now();
    const auto until = started + std::chrono::seconds(seconds);
    if (hz > 0) {
        const auto step = std::chrono::nanoseconds(1'000'000'000LL / hz);
        auto next = started + step;
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_until(next);
            next += step;
            samples += sample_once(handle, sink);
        }
    } else {
        std::this_thread::sleep_until(until);
    }

    g_stop.store(true);
    worker.join();
    CloseHandle(handle);
    return {g_work.load(), samples};
}

// ПАРНЫЙ замер, а не «база один раз в начале». Машина за прогон уезжает — греется,
// просыпается чужая задача, — и одна база на всю таблицу даёт просадку с неправильным
// знаком: сэмплированный прогон оказывается «быстрее» базы. Поэтому база меряется
// ВПЛОТНУЮ к своему замеру, и сравниваются они только друг с другом.
//
// Медиана отношений, а не среднее: один выброс не должен решать за всю таблицу.
struct verdict {
    double loss_pct = 0;
    std::uint64_t samples = 0;
};

verdict paired(int hz, int seconds, int pairs) {
    std::vector<double> losses;
    std::uint64_t samples = 0;
    losses.reserve(static_cast<std::size_t>(pairs));

    for (int i = 0; i < pairs; ++i) {
        const result base = run(0, seconds);
        const result with = run(hz, seconds);
        samples = with.samples;
        losses.push_back(100.0 *
                         (1.0 - static_cast<double>(with.work) / static_cast<double>(base.work)));
    }

    std::sort(losses.begin(), losses.end());
    return {losses[losses.size() / 2], samples};
}

}  // namespace

// ── Прямой замер окна ────────────────────────────────────────────────────────
// Замер через пропускную способность упирается в шум стенда (±3%), и на разумных
// частотах эффект в него проваливается — получается «не смог измерить», а не число.
//
// Меряем то, что происходит на самом деле: сколько поток простоял между Suspend и
// Resume. Отсюда цена считается арифметикой, а не вычитанием двух шумных величин:
//   доля = окно * частота
// Это и есть та часть кадра, которую профайлер забирает у сервера.
void measure_window(int seconds) {
    g_work.store(0);
    g_stop.store(false);

    std::thread worker{victim};
    const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                     FALSE,
                                     GetThreadId(worker.native_handle()));

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);

    std::vector<std::byte> sink(k_frames * k_frame_bytes);
    std::vector<double> window_us;

    // Каденция держится СПИНОМ, а не sleep: у Windows гранулярность сна ~15 мс, и на
    // `sleep_for(200мкс)` замер превращается в двести измерений вместо тысяч, причём
    // холодных — жертва успевает уехать с ядра, и мерится уже не окно, а пробуждение.
    const auto spin_us = [&freq](double us) {
        LARGE_INTEGER from{};
        QueryPerformanceCounter(&from);
        const auto ticks = static_cast<long long>(us * static_cast<double>(freq.QuadPart) / 1e6);
        LARGE_INTEGER now{};
        do { QueryPerformanceCounter(&now); } while (now.QuadPart - from.QuadPart < ticks);
    };

    // Целевая частота — та, на которой профайлер и будет работать.
    constexpr double k_period_us = 1000.0;  // 1 кГц

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) {
        LARGE_INTEGER a{};
        LARGE_INTEGER b{};
        QueryPerformanceCounter(&a);
        sample_once(handle, sink);
        QueryPerformanceCounter(&b);
        const double us = 1e6 * static_cast<double>(b.QuadPart - a.QuadPart) /
                          static_cast<double>(freq.QuadPart);
        window_us.push_back(us);
        if (us < k_period_us) { spin_us(k_period_us - us); }
    }

    g_stop.store(true);
    worker.join();
    CloseHandle(handle);

    std::sort(window_us.begin(), window_us.end());
    const auto at = [&](double q) { return window_us[static_cast<std::size_t>(q * static_cast<double>(window_us.size() - 1))]; };

    std::printf("Окно остановки: %zu замеров\n", window_us.size());
    std::printf("  медиана %.2f мкс   p95 %.2f мкс   p99 %.2f мкс   макс %.2f мкс\n\n",
                at(0.50), at(0.95), at(0.99), window_us.back());

    std::printf("Отсюда доля кадра, которую забирает профайлер:\n");
    std::printf("%8s %14s %14s\n", "Гц", "по медиане", "по p99");
    for (const int hz : {100, 250, 500, 1000, 2000, 4000}) {
        std::printf("%8d %13.3f%% %13.3f%%\n",
                    hz,
                    100.0 * at(0.50) * hz / 1e6,
                    100.0 * at(0.99) * hz / 1e6);
    }
}

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 2;
    const int pairs = argc > 2 ? std::atoi(argv[2]) : 5;

    measure_window(seconds);

    if (pairs > 0) {
        std::printf("\nСквозная проверка через пропускную способность\n");
        std::printf("Парный замер, медиана %d пар по %d с на сторону\n\n", pairs, seconds);
        std::printf("%8s %10s %12s\n", "Гц", "снимков", "просадка");
        for (const int hz : {1000, 4000}) {
            const verdict v = paired(hz, seconds, pairs);
            std::printf("%8d %10llu %11.2f%%\n", hz, v.samples, v.loss_pct);
        }
        std::printf("\nОтрицательное значение = эффект ниже шума стенда (~3%%).\n");
    }
    return 0;
}
