cmake_minimum_required(VERSION 3.30)
project(KR_GRAFTED_PROFILER CXX)

# ── Что здесь собирается ─────────────────────────────────────────────────────
#
#   krprof_core   чистое ядро: дерево стеков, символы, атрибуция, эмиттеры.
#                 Движка не знает вовсе — потому и проверяется тестами без игры.
#   krprof        сам плагин: <игра>/grafted/KRPROF.grafted.dll. Ядро + сэмплер.
#   krprof_tests  gtest поверх ядра.
#
# Ядро отделено от плагина не ради красоты: плагину нужен clang >= 21 и graft, а ядру —
# ничего. Разъедини их — и цикл «правка → тест» перестаёт зависеть от того и другого.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KRPROF_BUILD_PLUGIN "собирать сам плагин (нужен graft и clang >= 21)" OFF)
option(KRPROF_BUILD_TESTS  "собирать тесты ядра" ON)

# ── Ядро ─────────────────────────────────────────────────────────────────────
add_library(krprof_core STATIC
    SRC/attribution.cpp
    SRC/call_tree.cpp
    SRC/folded.cpp
    SRC/sidecar.cpp
    SRC/speedscope.cpp
    SRC/modmap.cpp
    SRC/analysis.cpp
    SRC/html.cpp
    SRC/trace.cpp
)
target_include_directories(krprof_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/INCLUDE")

target_compile_options(krprof_core PRIVATE
    $<$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>:/W4 /utf-8 /permissive- /EHsc>
    $<$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,GNU>:-Wall -Wextra -Wpedantic>
)

# ── Тесты ────────────────────────────────────────────────────────────────────
if(KRPROF_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        EXCLUDE_FROM_ALL)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(krprof_tests
        TESTS/attribution_test.cpp
        TESTS/call_tree_test.cpp
        TESTS/symbols_test.cpp
        TESTS/folded_test.cpp
        TESTS/sidecar_test.cpp
        TESTS/speedscope_test.cpp
        TESTS/modmap_test.cpp
        TESTS/analysis_test.cpp
        TESTS/html_test.cpp
        TESTS/flame_test.cpp
        TESTS/trace_test.cpp
        TESTS/episodes_test.cpp
    )
    target_link_libraries(krprof_tests PRIVATE krprof_core GTest::gtest_main)
    target_compile_options(krprof_tests PRIVATE
        $<$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>:/utf-8 /EHsc>)

    include(GoogleTest)
    gtest_discover_tests(krprof_tests)
endif()

# ── Замеры ───────────────────────────────────────────────────────────────────
# В ALL не входит: обычная сборка — это ядро и тесты, а мерить нужно, когда мерят.
# Игра не нужна — цена окна остановки от содержимого потока не зависит.
add_executable(krprof_bench EXCLUDE_FROM_ALL BENCH/suspend_bench.cpp)
target_compile_options(krprof_bench PRIVATE
    $<$<STREQUAL:$<CXX_COMPILER_FRONTEND_VARIANT>,MSVC>:/utf-8 /EHsc /O2>)

# ── Плагин ───────────────────────────────────────────────────────────────────
# Отдельно и по требованию: тесты ядра не должны ждать ни graft, ни clang 21.
#   cmake -B build-plugin -G Ninja -DKRPROF_BUILD_PLUGIN=ON ^
#         -DGRAFT_SOURCE_DIR=E:/source/KR_GRAFTED_PTY ^
#         -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if(KRPROF_BUILD_PLUGIN)
    if(NOT GRAFT_SOURCE_DIR)
        message(FATAL_ERROR "нужен -DGRAFT_SOURCE_DIR=<путь к KR_GRAFTED_PTY>")
    endif()
    include("${GRAFT_SOURCE_DIR}/cmake/graft.boot.cmake")
    graft_import(graft "${GRAFT_SOURCE_DIR}")

    graft_plugin(krprof
        NAME    KRPROF
        VERSION 1
        SOURCES SRC/plugin.cpp
                SRC/engine.cpp
                SRC/debug_info.cpp
                SRC/sampler.cpp
                SRC/native_stack.cpp
        MODULES 3_Game)
    # graft::mt — единственный, кто уже нашёл скриптовую машину: текущий поток, массив
    # кадров и их раскладку. Повторять этот поиск значило бы завести второй набор ошибок.
    # dbghelp — символы для стека ОС. У самой игры их нет (PDB отсутствует, три
# экспорта), но системные библиотеки имена отдают, и уже с demangle.
target_link_libraries(krprof PRIVATE krprof_core graft::mt dbghelp)
endif()
