# Tests — 测试与性能基准使用指南

Pictor 的全部单元测试与性能基准编译进**同一个**可执行目标 `InfraTests`，
刻意**不注册 CTest**：configure 之后只会多出一个构建目标，不会为每个测试
生成独立目标。运行时通过子命令在单元测试与性能基准之间切换：

```
InfraTests            运行全部单元测试（等价于 `InfraTests test`）
InfraTests test ...   单元测试
InfraTests bench ...  性能基准
InfraTests all        先跑单元测试，再跑性能基准
InfraTests --help     用法说明
```

结果**同时输出到控制台与文件**：

| 内容 | 控制台 | 文件（默认在二进制同目录） |
|---|---|---|
| 单元测试 | gtest 风格 `[ RUN ]/[ OK ]/[ FAILED ]` + 汇总 | `test_results.txt`（镜像）、`test_results.xml`（JUnit） |
| 性能基准 | 场景对比表（ns/op、吞吐、相对基线比值） | `bench_report.json`（对比）、`bench_report.md`（报表）、`bench_raw.json`（原始测量） |

## 1. 构建

测试只依赖 `Infra` 模块（仅标准库），关闭应用即可离线构建：

```bash
# 日常开发（调试）
cmake -S . -B build-infra -DPICTOR_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-infra --target InfraTests

# 性能基准务必用 Release（Debug 数据没有参考意义）
cmake -S . -B build-release -DPICTOR_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target InfraTests
```

开关说明：

- `PICTOR_BUILD_APP=OFF`：跳过应用（渲染器/平台，需联网拉取 Vulkan-Headers 与 DXC）。
- `PICTOR_BUILD_TESTS=OFF`：完全跳过 `InfraTests` 目标（默认 `ON`）。

构建产物为单一二进制 `build-*/tests/InfraTests`（Windows 下为 `InfraTests.exe`）。

## 2. 运行单元测试

```bash
./build-release/tests/InfraTests            # 或 `InfraTests test`
```

支持选项（对齐 GoogleTest 常用开关）：

| 选项 | 说明 |
|---|---|
| `--filter=<substr>` | 只运行名称包含子串的用例，如 `--filter=FlatHashTable_` |
| `--seed=<n>` | 按种子打乱用例顺序（默认 0 不打乱），失败可复现 |
| `--repeat=<n>` | 整体重复运行 n 次（排查偶发失败） |
| `--list-tests` | 列出全部用例名后退出 |
| `--out-dir=<path>` | 报告输出目录（默认二进制所在目录） |

退出码：全部通过为 0，任一失败为 1（可用于 CI 判定）。

### 查看结果

- **控制台**：每个用例一行 `[ RUN ]`/`[ OK ](耗时)`，失败用例会打印断言表达式、
  期望值/实际值、文件与行号；结尾汇总「N test(s)、M check(s)、K failure(s)」。
- **`test_results.txt`**：控制台输出的完整镜像，便于事后翻阅。
- **`test_results.xml`**：JUnit 格式，可直接被 Jenkins/GitLab CI/GitHub Actions
  等工具解析；每个用例一个 `<testcase>`（含耗时），失败用例附 `<failure>` 详情。

## 3. 运行性能基准

```bash
./build-release/tests/InfraTests bench
```

支持选项：

| 选项 | 说明 |
|---|---|
| `--filter=<substr>` | 只运行名称包含子串的基准，如 `--filter=hashtable.flat` |
| `--min-time-ms=<n>` | per-op 类基准自动校准的目标总时长（默认 80 ms/个） |
| `--iterations=<n>` | bulk 类基准的固定迭代次数（默认 5，另有 1 次预热） |
| `--list` | 列出全部基准名与场景说明后退出 |
| `--out-dir=<path>` | 报告输出目录（默认二进制所在目录） |

完整运行（54 个基准）在 Release 下约需 5~10 秒。

### 基准清单与方法

两个套件：

- **`hashtable.*`**：`FlatHashTable` / `LinkedHashTable` 对比基线
  `std::unordered_map`（基线比值 = 1.00x）。
  - 插入：`insert.grow`（空表含扩容，128k/1m）、`insert.reserved`（先 reserve）、
    `insert.sequential`（升序最坏聚集模式）
  - 查找：`findHit` / `findMiss`（128k 预构建表，批量 1024 次/调用）
  - 删除：`eraseAll`（**净耗时** = 自身 − `insert.grow`，已抵消建表成本）
  - 其它：`iterate`（全量遍历）、`copy`（整体拷贝）、`mixed`（命中/插入/删除
    混合，批量 4096 次/调用）、`stringInsert` / `stringFindHit`（24 字符字符串键）
- **`rbtree.*`**：`IntrusiveRBTree` 对比基线 `std::set`。
  - 插入：`insert.random` / `insert.sorted` / `insert.reverse`（128k）
  - 查找：`findHit` / `findMiss` / `lowerBound`
  - 删除：`eraseAll`（净耗时）、`iterate`（中序遍历）、`mixed`

测量方法（参考 Google Benchmark 等成熟工具的做法）：

- 三种实现共享同一份**固定种子**生成的键序列，从完全相同的初始状态开始，
  保证公平对比与可复现。
- 短操作（查找/删除等）按**批量折算**：每次计时调用内完成 1024/4096 次基本
  操作，再除以批量得到 ns/op，摊薄计时框架自身开销。
- per-op 类基准先预热 2 次，再自动校准迭代次数使总测量时长接近
  `--min-time-ms`；bulk 类（整表插入/遍历等）先预热 2 次消化冷启动
  （页错误/分配器预热），再用固定迭代次数（默认 5，`--iterations` 可调）。
- 派生净耗时场景（`eraseAll` = 自身 − `insert.grow`/`insert.random`）对噪声
  敏感，其基础与派生基准自动使用更长采样（10 次迭代）。
- 结果统一折算为 **ns/op（每个基本操作的平均纳秒数）**，数值越小越好；
  `vs base` 列为相对标准库的比值（< 1.00x 表示更快）。

### 查看结果

- **控制台**：每个场景一张对比表（`impl / mean ns/op / throughput / vs base`）。
- **`bench_report.md`**：人类可读的 Markdown 报表，按套件汇总全部场景与比值，
  适合直接贴到文档或评审记录里。
- **`bench_report.json`**：机器可读的对比数据（环境信息、场景、各实现
  ns/op、吞吐、比值），适合脚本画图或入库对比历史。
- **`bench_raw.json`**：Bm 框架的原始测量（迭代次数、总耗时、单次均值），
  需要复查底层数据时使用。

> 示例：在 Release 下 `./build-release/tests/InfraTests bench --filter=hashtable.findHit`
> 只测命中查找；`bench --filter=rbtree` 只测红黑树套件。

## 4. 测试与基准的工程实践说明

- **先行为正确性、后性能**：单元测试覆盖全部 API、边界输入（空表/单元素/
  极端键/空字符串/移动后状态/非法装载因子）、异常安全（构造抛/拷贝抛/重哈希
  抛）、分配器记账（零逐元素分配、分配释放平衡），以及与标准库的
  **多种子随机差分测试**（哈希表对照 `std::unordered_map` 并核对插入顺序，
  红黑树对照 `std::set` 并逐步校验红黑不变量）。
- **可复现**：模糊测试与基准都使用固定种子；用例可 `--seed` 打乱、
  `--repeat` 复跑、`--filter` 定位。
- **CI 友好**：单一二进制 + 退出码语义 + JUnit XML 报告，便于接入任意 CI；
  无需 CTest，也就没有每测试一个目标的问题。
- 基准数据只在**同一台机器、同一个 Release 构建**之间可比；跑分时尽量关闭
  其它负载，必要时多次运行取稳定值。

## 5. 目录结构

```
tests/
  CMakeLists.txt           单一 InfraTests 目标（无 CTest 注册）
  TestMain.cpp             统一入口（test/bench/all 子命令分发）
  TestFramework.h          轻量测试框架（断言宏、过滤/种子/重复、txt+JUnit 报告）
  config.ini               serde 测试数据（经 TEST_DATA_DIR 定位）
  containers/              FlatHashTable / LinkedHashTable / IntrusiveRBTree 测试
  serde/                   Ini / SerdeEngine 测试
  benchmark/
    BenchmarkCommon.hpp    基准元数据、分组、对比报表（控制台/JSON/Markdown）
    BenchmarkMain.cpp      bench 子命令入口
    HashTableBenchmark.cpp 哈希表基准（基线 std::unordered_map）
    RBTreeBenchmark.cpp    红黑树基准（基线 std::set）
```
