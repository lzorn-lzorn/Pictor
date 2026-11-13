
# 命名规范
函数命名, 变量命名, 文件命名要有描述性, 少用缩写. 对于特殊前缀应该提前约定

## 代码命名
### 类型, TypeTrait, Concept
类型, TypeTrait, Concept 表示类型语义使用大驼峰.
除非与标准库兼容的 `using value_type = xxx;` 使用与标准库兼容的名称
```Cpp
template <typename Ty>
struct TypeA {
    using pointer = Ty*;
    using const_pointer = const Ty*;
    using value_type = Ty;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    using Tarits = Ty::Tarits;
};
```
使用特定的字母开头用于表示该类的特殊作用:
- I- : 接口类
- E- : 枚举

### 函数形参, 局部变量, 成员变量
全局常量, 函数形参, 变量, 成员变量表示值语义使用大驼峰命名. 表示名词语义


```Cpp
constexpr int32_t MaxSupportedNumber = 100; 
```
对于全局变量使用 G- 开头
```cpp
enum class EState {
    Start,
    End,
    Hold
};
```
并且应该使用 `enum class` 而不是 `enum`
使用特定的字母开头用于表示该值的特殊作用:
- G- : 全局对象

### 函数, 成员函数, lambda
函数, 成员函数, lambda 使用小驼峰且动词, 介词, 动词短语开头, 表示动作.

api 完整注释格式:
```
/**
 * @brief API 行为
 * @tparam 类型约束的输入
 *      \tparam1 类型参数1 含义;
 * @param 参数含义和有效输入范围
 *      \param1 参数1 含义; 有效输入范围
 *      \param2 参数2 含义; 有效输入范围
 *      ...
 * @return 返回值含义和有效返回范围
 * @note 注意事项
 */
```
非完整格式(并非每一个 api 都需要严格注释可以有以下简化)
```cpp
// @brief API 行为
```
或者
```cpp
// @note API 行为
```

### 命名空间
命名空间 (namespace) 应该使用 **一个小写单词** 表示. 虽然你不需要吝啬命名空间的使用, 但
也应该确实其含义以免多级命名空间过长的情况, 大致控制在三级(不用非要用满三级):

**主名称空间::系统名称空间::内部模块名称空间**
## 目录
目录统一使用全小写的下划线来命名, 因为有的文件系统是不区别大小写的, 统一使用小写来防止不必
要的问题

对于 infra 目录下的所有库, 必须使用 C++11 的兼容开发. 即必须支持 C++11 同时也要有针对 C++20 以上版本的优化.
对于其他目录则基于 C++23 开发即可.