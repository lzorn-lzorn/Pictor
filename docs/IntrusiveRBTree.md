# IntrusiveRBTree 使用指南

侵入式红黑树，语义对齐 `std::set`（键唯一、有序迭代、`O(log n)` 插入/删除/查找）。
与 `std::set` 的关键区别：**容器不拥有节点**，节点内存与生命周期由调用方管理。

- 头文件：`infra/containers/IntrusiveRBTree.hpp`（类名 `IntrusiveRBTree`）
- 参考实现：`boost::intrusive::rbtree`（compact 节点表示，颜色打包进父指针低 bit）
- 适用场景：节点已经需要被多处持有/索引、希望零拷贝、或需要把同一个对象同时挂进
  多个容器（如按多种键排序）时。

---

## 1. 节点类型要求

值类型 `T` 必须满足：

1. **公开继承** `RBNodeBase`；
2. **无需可复制/移动**（容器从不复制/移动元素）；若类型可复制，副本为**未链接**状态、需自行插入——元素在容器内期间地址必须稳定；
3. **生命周期长于容器**：节点必须先于容器销毁前被移除或由调用方释放。

```cpp
#include "infra/containers/IntrusiveRBTree.hpp"

struct Entity : RBNodeBase
{
    int    Id   = 0;
    std::string Name;

    // std::less<> 默认按 operator< 排序
    bool operator<(Entity const& Other) const { return Id < Other.Id; }
};
```

---

## 2. 快速上手

```cpp
#include <list>
#include "infra/containers/IntrusiveRBTree.hpp"

IntrusiveRBTree<Entity> Tree;   // 默认按 std::less<> 升序
std::list<Entity> Storage;      // 节点的真实所有者（地址稳定）

// 插入
for (int Id : {3, 1, 2})
{
    Storage.emplace_back();
    Storage.back().Id = Id;
    auto [It, Inserted] = Tree.insert(Storage.back()); // pair<iterator,bool>
    // Inserted == false 表示键已存在
}

// 遍历（升序）
for (auto It = Tree.cbegin(); It != Tree.cend(); ++It)
{
    // *It 得到 Entity&（const 迭代器得到 Entity const&）
    // It-> 得到 Entity*
}

// 查找 / 删除
if (auto It = Tree.find(2); It != Tree.end())
{
    Tree.erase(It);            // 返回被删元素之后的迭代器
}
Tree.erase(3);                 // 按键删除，返回删除个数（0 或 1）
```

---

## 3. 异构查找（直接用键查找，无需构造节点）

默认比较器 `std::less<>` 是透明的，只要为「键类型 × 节点类型」补上 `<`，
即可用裸键做 `find/getLowerBound/...`，而无需构造一个临时的 `T`：

```cpp
bool operator<(Entity const& A, int B) { return A.Id < B; }
bool operator<(int A, Entity const& B) { return A < B.Id; }

Tree.find(2);
Tree.count(2);
Tree.contains(2);
Tree.getLowerBound(2);
Tree.getUpperBound(2);
Tree.getEqualRange(2);   // pair<iterator,iterator> [lower, upper)
```

---

## 4. 自定义比较器

传入一个**透明**比较器即可既控制排序方向、又保留异构查找能力：

```cpp
struct Descending
{
    bool operator()(Entity const& A, Entity const& B) const { return A.Id > B.Id; }
    bool operator()(Entity const& A, int B) const { return A.Id > B; }
    bool operator()(int A, Entity const& B) const { return A > B.Id; }
};

IntrusiveRBTree<Entity, Descending> Tree;   // 降序
```

若只需同构比较，也可直接用 `std::greater<Entity>` 等。

---

## 5. API 一览

| 分类 | 成员 |
|---|---|
| 容量 | `empty()`、`size()` |
| 观察器 | `getKeyCompare()`、`getValueCompare()` |
| 迭代器 | `begin/end`、`cbegin/cend`、`rbegin/rend`、`crbegin/crend` |
| 插入 | `insert(T&)` → `pair<iterator,bool>`；`insert(const_iterator hint, T&)` |
| 删除 | `erase(iterator)`、`erase(const_iterator)`、`erase(first,last)` → 返回后继；`erase(Key)` → `size_type` |
| 清空 | `clear()`、`clear_and_dispose(Disposer)` |
| 交换 | `swap(Tree&)`；自由函数 `swap(a, b)`（ADL） |
| 查找 | `find`、`count`、`contains`、`getLowerBound`、`getUpperBound`、`getEqualRange`（均异构 + const/非 const 重载） |
| 比较 | `==`、`!=`、`<`、`<=`、`>`、`>=`（`==` 用排序等价关系，仅要求 `operator<`） |
| 调试 | `validate()` |

类型别名：`value_type/key_type/size_type/difference_type/reference/pointer/iterator/const_iterator/reverse_iterator`。

---

## 6. 迭代器

- 双向迭代器（`std::bidirectional_iterator_tag`），支持前向与反向遍历。
- `*it` 返回 `T&`、`it->` 返回 `T*`；`const_iterator` 对应 `const` 版本。
- `iterator` 可隐式转换为 `const_iterator`，反之不可。
- `end()` 是 header 哨兵，`--end()` 得到最大元素，`++` 到 `end()` 表示结束。
- 删除元素只使指向**被删节点**的迭代器失效，其余迭代器保持有效。

```cpp
// 反向遍历
for (auto It = Tree.crbegin(); It != Tree.crend(); ++It) { /* ... */ }
```

---

## 7. 生命周期与所有权

- **插入**会重置节点的链接，因此被 `erase`/`clear` 过的节点可安全地再次 `insert`。
- `erase(iterator)`/`clear()` 会把被移除节点的 `left/right/parent` 置空，使其回到干净、
  可复用状态；但**不调用析构**。
- `clear()`：清空容器并重置每个节点（`O(n log n)`）。
- `clear_and_dispose(Disposer)`：逐元素调用 `Disposer(&node)` 后再清空，用于需要
  由容器代为释放的场景：

```cpp
Tree.clear_and_dispose([](Entity* E) { delete E; });
```

- `swap` 交换两棵树（`O(1)`，含比较器与 size）。

---

## 8. 复杂度

| 操作 | 复杂度 |
|---|---|
| `insert` / `erase` / `find` / `count` / `contains` | `O(log n)` |
| `getLowerBound` / `getUpperBound` / `getEqualRange` | `O(log n)` |
| 单次迭代器 `++` / `--` | 均摊 `O(1)` |
| `clear` | `O(n log n)` |
| `clear_and_dispose` | `O(n log n)` |
| `swap` / `empty` / `size` | `O(1)` |
| `validate` | `O(n)` |

---

## 9. 线程安全

容器**非线程安全**。并发修改，或在另一线程读取时修改，均为未定义行为。
需要同步时请由调用方加锁（红黑树本身不含任何锁）。

---

## 10. 调试

`validate()` 以 `O(n)` 校验全部红黑不变量，返回 `false` 表示结构被破坏：

- 根为黑；
- 不存在「红-红」父子；
- 所有叶路径黑高相等；
- 父子指针一致、`size()` 与实际节点数一致。

建议在 Debug 构建的测试/断言中周期性调用，用于尽早暴露误用（如把节点重复插入两棵树）。
