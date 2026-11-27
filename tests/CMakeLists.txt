# Tests — 全部单元测试与性能基准合并为【单一】可执行目标 InfraTests。
#
# 刻意不使用 CTest 注册（避免为每个测试生成额外构建目标）；所有测试与基准
# 编译进同一个二进制，运行时通过子命令选择：
#
#   InfraTests           运行全部单元测试（等价于 `InfraTests test`）
#   InfraTests test ...  单元测试（--filter / --seed / --repeat / --list-tests / --out-dir）
#   InfraTests bench ... 性能基准（--filter / --min-time-ms / --iterations / --list / --out-dir）
#   InfraTests all       先跑单元测试，再跑性能基准
#
# 测试数据（config.ini）通过 TEST_DATA_DIR 定位；测试与基准报告默认写入
# TEST_OUTPUT_DIR（本目录在构建树中的对应位置），也可用 --out-dir 覆盖。
#
# 目录结构：
#   TestMain.cpp            统一入口（子命令分发）
#   TestFramework.h         轻量测试框架（断言、注册、控制台 + txt/XML 报告）
#   containers/             FlatHashTable / LinkedHashTable / IntrusiveRBTree 测试
#   serde/                  Ini / SerdeEngine 测试（依赖 tests/config.ini）
#   benchmark/              性能基准：哈希表 vs std::unordered_map，红黑树 vs std::set

add_executable(InfraTests
    TestMain.cpp
    containers/FlatHashTableTests.cpp
    containers/LinkedHashTableTests.cpp
    containers/IntrusiveRBTreeTests.cpp
    serde/IniFileReaderTests.cpp
    serde/SerdeEngineTests.cpp
    benchmark/BenchmarkMain.cpp
    benchmark/HashTableBenchmark.cpp
    benchmark/RBTreeBenchmark.cpp
)

target_link_libraries(InfraTests PRIVATE Infra)
target_include_directories(InfraTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(InfraTests PRIVATE cxx_std_23)
target_compile_definitions(InfraTests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    PICTOR_TEST_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    PICTOR_TEST_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)

if(MSVC)
    target_compile_options(InfraTests PRIVATE /W4 /utf-8)
else()
    target_compile_options(InfraTests PRIVATE -Wall -Wextra)
endif()
