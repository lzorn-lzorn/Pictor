# Ini 解析规则与支持的语法

本文档描述 `infra/serde/parser/Ini.h` 中 Ini 解析器的全部规则与语法支持。所有行为均可经
`IniPolicy` 逐项开关,策略默认值即为文档中标注的"默认"行为。

- 头文件: `infra/serde/parser/Ini.h`(解析器)/ `infra/serde/parser/IniFile.h`(文件层)
- 独立用法: 直接使用 `Ini` 读写配置文件(推荐,配置文件仍是 Ini 的主流用法)
- 序列化后端: 经 `infra/serde/SerdeIniEngine.hpp` 作为 `SerdeEngine` 的 INI 后端参与反射序列化

---

## 1. 基本结构

一个 INI 文档由「行」组成,支持三类结构元素:

| 元素 | 形式 | 说明 |
|---|---|---|
| 注释行 | `# ...`、`; ...`(首字符为注释符) | 整行被忽略 |
| Section 头 | `[Name]`、`["Name"]`、`[Child : Parent]` | 后续 Key-Value 归入该 Section |
| Key-Value 行 | `Key = Value` 或 `Key : Value` | 键值对 |

- 空行与纯空白行被忽略(但会保留在行缓冲区 / 注释保真结构中,见 §8)。
- 行尾 `\r`(CRLF)自动剥除,`\n` 为行分隔符。
- 注释符(`#` `;`)与分隔符(`=` `:`)可经策略替换(`AnnotationChars` / `DelimiterChars`)。
- Key-Value 必须出现在某个 Section 内;Section 之外的 Key-Value 报错
  `KeyValueOutsideSection`。
- 输入默认经 **UTF-8 校验**(`ValidateUtf8 = true`),非法序列报 `InvalidUtf8`。

```ini
; 注释
[General]            ; 普通 Section 头
app_name = HelloVulkan
build = 20261118

["Name With Spaces"] ; 引号包裹的 Section 名
x = 1

[Child : General]    ; Section 继承: Child 回退到 General
debug = false
```

---

## 2. Section 头

### 2.1 普通形式 `[Name]`

- 名称两端空白被裁剪。
- 默认只允许:字母、数字,以及附加字符 `_ . - |`(`ExtraSectionNameChars`,可替换)。
- 空名或含非法字符 → 按 `SectionErrorHandle` 处理:
  - `Drop`(默认):该畸形 Section 及其下所有 Key-Value 被静默忽略;
  - `TransferToDefault`:其下 Key-Value 并入默认 Section(名 `DefaultSectionName`,默认 `"Default"`)。
- 闭合 `]` 之后不允许任何多余内容,否则报 `TrailingContentInSectionHeader`;
  缺失 `]` 报 `UnclosedSectionHeader`。
- `AllowAnySectionName = true` 时名称只要求非空(引擎侧使用该模式)。

### 2.2 引号形式 `["Name"]`

- 名称可为任意非空文本(不做事符集校验);支持 `\"` 与 `\\` 转义。
- 未闭合引号报 `UnclosedQuotedSectionName`。
- 序列化时,凡不满足普通形式字符集(或含首尾空白)的 Section 名会自动写回引号形式。

### 2.3 继承形式 `[Child : Parent]`

- 由 `EnableSectionInheritance = true`(默认)启用。
- `Child` 的键优先;缺失时沿 `Parent` 链回退查找(`hasKey` / `get` 均继承感知)。
- 父名可为任意已定义/后续定义的 Section 名;父名非法或为空报 `InvalidParentSectionName`。
- 查找带环保护:父链成环时按"未找到"处理。
- 引号名同样支持继承: `["Child Name" : Parent]`。
- `keys(Section)` 只列出该 Section 自身定义的键(不含继承)。
- 序列化写回 `[Child : Parent]`;同一 Section 多次出现时首次出现的父声明生效。

---

## 3. Key(键名)

- 行首空白与键尾空白被裁剪;键内连续空白压缩为单个空格(`InteriorSpaceInKey = true`
  时允许,如 `Var Name = 1` 得键 `"Var Name"`;关闭时该行报 `InteriorSpaceInKey`)。
- **始终禁止**的字符(`ForbiddenKeyChars`,默认 `" ' \ [ ] { } ( )`)遇之报 `InvalidKeyCharacter`。
- 开启 `KeyNameValidation = true`(默认)时,键只能由字母、数字、下划线(及策略允许的键内空白)
  组成,且**不能以数字开头**(报 `DigitLeadingKey`);关闭后仅受禁止字符集约束。
- 大小写策略 `KeyCaseMode`: `Default`(保留原样)/ `UpperCase` / `LowerCase`;
  **存储与查找两侧都做折叠**(折叠后键名即为 `keys()` 返回的内容)。
- 注释符出现在分隔符之前 → 该行无法成为 Key-Value,报 `CommentInKey`。
- 严格模式(`StrictInspection = true`,默认):
  - 必须出现分隔符且键非空,否则分别报 `MissingDelimiter` / `EmptyKey`;
- 非严格模式:
  - 缺分隔符 → 值取默认值 `DefaultValue`(默认 `"NULL"`),如 `bare` ⇒ 键 `bare`、值 `"NULL"`;
  - 分隔符后空键 → 键取默认值 `DefaultValue`,如 `= v` ⇒ 键 `"NULL"`、值 `"v"`。

---

## 4. Value(值)

### 4.1 一般规则

- 分隔符后前导空白裁剪;引号外的行尾空白裁剪(引号内的空白保留)。
- 空值合法: `key =` ⇒ 空字符串。
- 引号外出现注释符 ⇒ 其后为行尾注释,不进入值;出现第二个分隔符 ⇒ 报 `SecondDelimiterInValue`
  (引号内不受此限)。
- 双引号成对切换,引号本身不进入值;`"a; b # c"` 中的注释符与分隔符按普通字符保留。

### 4.2 转义序列(引号内外均生效)

| 写法 | 结果 |
|---|---|
| `\n` `\t` `\r` | 换行 / 制表 / 回车 |
| `\a` `\b` `\f` `\v` | 响铃 / 退格 / 换页 / 垂直制表 |
| `\0` | NUL 字符 |
| `\xNN` | 1~2 位十六进制字节(非法报 `InvalidEscapeSequence`) |
| `\"` `\'` `\\` | 引号 / 单引号 / 反斜杠本身 |
| `\;` `\#` | 字面注释符(不触发行尾注释) |
| `\=` `\:` | 字面分隔符(不触发分隔符错误) |
| 其他 `\c` | 丢弃反斜杠,`c` 按普通字符处理 |

示例: `sep = a\=b` ⇒ `a=b`; `semi = a\;b ; tail` ⇒ `a;b`(tail 为注释)。

### 4.3 多行续行值

- 由 `EnableLineContinuation = true`(默认)启用。
- 值以**未转义的反斜杠**结尾(奇数个连续 `\`)时,下一行作为值的一部分;
  反斜杠前的空白保留(用于拼接词间空格),续行内容两端空白裁剪后直接拼接(不插入换行)。
- 续行行自身也可再以 `\` 结尾继续续行。
- 续行遇到新的 Section 头时防呆终止(累积值末尾空白被裁剪),该头行按普通行处理。

```ini
[S]
msg = first part \
second part      ; ⇒ "first part second part"
```

### 4.4 列表值

- 由 `EnableListValues = true`(默认)启用;整个值形如 `[...]`(经裁剪后)才按列表解析。
- 元素以逗号切分(引号内的逗号除外),元素两端空白裁剪,引号剥除、转义按 §4.2 处理;
  `[]` 为空列表;元素级转义非法或引号未闭合时,整个值退回按普通字符串存储。
- 存储类型为 `IniStringList`;`get<std::string>` 返回 `[a, b, c]` 形式的文本。

```ini
[S]
nums  = [1, 2, 3]
words = [a, "b, c", d]   ; ⇒ {"a", "b, c", "d"}
```

### 4.5 重复键

- `DuplicateKeysAsList = false`(默认):后者覆盖前者(保留首次出现处的注释);
- `DuplicateKeysAsList = true`: 重复键收集为 `IniStringList`(标量与列表元素均可混入)。

### 4.6 环境变量插值

- 由 `EnableInterpolation = true`(默认)启用:值中的 `${VAR}` 在解析时替换为环境变量 `VAR`。
- 未闭合的 `${` 原样保留;未定义变量:`StrictInterpolation = false`(默认)时保留原文,
  否则报 `UnresolvedInterpolation`。
- 仅作用于值;`set(...)` 写入路径同样执行插值。

---

## 5. 值类型推断(存储模型)

解析出的值按以下顺序推断并存入 `IniVariant`:

```cpp
using IniStringList = std::vector<std::string>;
using IniVariant = std::variant<bool, std::int64_t, double, std::string, IniStringList>;
```

1. 形如 `[...]` 且 `EnableListValues` → `IniStringList`;
2. `true/yes/on`、`false/no/off`(大小写不敏感)→ `bool`;
3. 完整可解析的整数 → `std::int64_t`;完整可解析的浮点 → `double`;
4. 其余 → `std::string`。

`get<T>` 支持目标类型间转换: `bool` / 任意整型 / 任意浮点 / `std::string` /
`IniStringList` / `std::vector<整型或浮点或string>`,失败返回调用方给出的默认值。

---

## 6. IniPolicy 全部选项与默认值

| 选项 | 默认 | 含义 |
|---|---|---|
| `KeyCaseMode` | `ECaseMode::Default` | 键名大小写折叠(存储与查找一致) |
| `StrictInspection` | `true` | 要求"键 + 分隔符"齐备 |
| `KeyNameValidation` | `true` | 键只允许字母/数字/下划线且不得数字开头 |
| `InteriorSpaceInKey` | `true` | 允许键内空白(压缩为单空格) |
| `SectionErrorHandle` | `ESectionErrorHandle::Drop` | 畸形 Section 名:丢弃 / 并入默认 Section |
| `AllowAnySectionName` | `false` | true 时 Section 名只要求非空 |
| `EnableSectionInheritance` | `true` | `[Child : Parent]` 继承 |
| `DefaultSectionName` | `"Default"` | 默认 Section 名 |
| `EnableListValues` | `true` | `[a, b, c]` 列表值 |
| `DuplicateKeysAsList` | `false` | 重复键收集为列表 |
| `EnableLineContinuation` | `true` | 行尾 `\` 续行 |
| `EnableInterpolation` | `true` | `${VAR}` 环境变量插值 |
| `StrictInterpolation` | `false` | 未定义变量视为错误 |
| `DefaultValue` | `"NULL"` | 非严格模式的默认键名/默认值 |
| `ValidateUtf8` | `true` | 解析前校验 UTF-8 |
| `AnnotationChars` | `"#;"` | 注释符集合 |
| `DelimiterChars` | `"=:"` | 分隔符集合 |
| `ExtraSectionNameChars` | `"_.-|"` | Section 名附加字符 |
| `ForbiddenKeyChars` | `"\"'\\[]{}()"` | 键中始终禁止的字符 |

---

## 7. 错误模型

所有失败以 `std::expected<..., IniError>` 返回;`IniError` 含错误码、行号(1 起,文件层错误为 0)
与上下文文本,`IniError::asString()` 输出英文提示:

```
line 2: missing delimiter: no_delimiter
cannot open file: path/to/file.ini
```

| 错误码 | 场景 |
|---|---|
| `CannotOpenFile` / `ReadFailed` / `WriteFailed` | 文件层 |
| `InvalidUtf8` | 输入不是合法 UTF-8 |
| `UnclosedSectionHeader` / `TrailingContentInSectionHeader` | `[Name` 缺 `]` / `[Name] x` |
| `InvalidSectionName` | 空名或非法字符(检查器) |
| `UnclosedQuotedSectionName` | `["Name` 缺闭合引号 |
| `InvalidParentSectionName` | `[Child : 非法父名]` |
| `CommentInKey` | 分隔符前出现注释符 |
| `SecondDelimiterInValue` | 值中(引号外)出现第二个分隔符 |
| `InvalidEscapeSequence` | 非法 `\xNN` |
| `KeyValueOutsideSection` | Section 之外的 Key-Value |
| `EmptyKey` / `MissingDelimiter` | 严格模式的空键 / 缺分隔符 |
| `DigitLeadingKey` | 键以数字开头 |
| `InvalidKeyCharacter` / `InteriorSpaceInKey` | 键含禁止字符 / 键内空白被策略禁止 |
| `UnresolvedInterpolation` | 严格插值下 `${VAR}` 未定义 |
| `UnsupportedValueType` | `set<T>` 对未注册自定义类型 |

---

## 8. 注释与格式的往返保真

- 解析时逐行保存原文到 `lineBuffer()`(含注释与空行,已去 `\r`);`releaseLineBuffer()` 移动释放。
- 每个 Section 头前、每个键值对前的注释/空行原文分别存入 `IniSection::HeaderLines` 与
  `IniEntry::LeadingLines`;文件末尾的注释存入 `TailLines`。
- `serialize()` / `flash()` 按 头注释 → `[Name]` → (键前注释 → `Key = Value`) 的顺序写回,
  **注释与空行位置完整保留**;`erase` 连带删除该键的前导注释,`set` 覆盖值但保留原有注释。
- 写回时的自动转义与引号:
  - 值:含 `, ; # = : " \` 或控制字符、首尾空白、形如 `[..]` 时引号包裹,
    转义规则与 §4.2 成对;
  - 列表: `[a, "b, c", d]` 形式,元素按需引号包裹;
  - Section 名:不满足字符集或含首尾空白时写回 `["..."]`。
- `flash()` 同时把全文按行写回 `lineBuffer()`;任何 `set`/`erase`/`eraseSection` 都会使
  行缓冲区失效,直到下一次 `parse` 或 `flash`。

---

## 9. 公开 API 摘要

```cpp
class Ini
{
public:
    Ini(); explicit Ini(IniPolicy Policy);

    // 解析(全部返回 std::expected)
    parse(std::string_view);          // 内存文本
    parse(std::istream&);             // 流式逐行解析
    load(path);                       // 读文件 + 解析
    loadMapped(path);                 // mmap 读取(Windows 文件映射 / POSIX mmap / 其他退化)
    static fromString(text); static fromFile(path);

    // 写回
    save(path); save();               // 无参 save 写回 load/loadMapped 的来源文件

    // 查询
    sections(); keys(section); section(name); hasSection(name); hasKey(name, key);

    // 取值
    template<class T> T get(section, key, T Default = {}); // 内建转换 + 自定义类型
    std::optional<IniVariant> getVariant(section, key);

    // 写入 / 删除
    set(section, key, value);         // string_view / 内建类型 / vector / 自定义类型
    template<class T> void registerType(ToString, FromString); // 自定义类型 <-> 字符串
    erase(section, key); eraseSection(section);

    // 其他
    merge(other); checker(); lastError(); isLoaded();
    serialize(); flash(); lineBuffer(); releaseLineBuffer();
};
```

示例:

```cpp
Ini Doc;
if (auto R = Doc.load("config.ini"); !R) { /* IniError::asString(R.error()) */ }

const std::string Name = Doc.get<std::string>("General", "app_name", "fallback");
const int         Fps  = Doc.get<int>("Graphics", "max_fps");

Doc.set("Window", "width", 1920);
Doc.erase("Audio", "mute");
Doc.save();                       // 写回 config.ini
const std::string Text = Doc.flash(); // 同时更新行缓冲区
```

---

## 10. 文件层与 SerdeEngine 后端

- 文件层(`infra/serde/parser/IniFile.h`)与解析解耦:
  `readIniFile`(剥 UTF-8 BOM)/ `writeIniFile` / `mapIniFile` + `IniMappedFile`(RAII 视图)。
- `SerdeIniEngine`(`infra/serde/SerdeIniEngine.hpp`)把 `SerdeADT` 映射为 Ini:
  - 根对象每个 object 成员 → `[Section]`;其成员 → `Key = Value`;
  - 嵌套 object → 点分键 `parent.child`,读回时展开回嵌套对象;
  - `SerdeArray` → `[a, b, c]`(元素推断 int/double/string,支持 `vector<int>` 往返);
  - null → 空值;binary / datetime 与数组嵌套对象不支持(明确抛错);
  - 引擎读写两侧使用宽容策略(任意 Section 名、键名不校验);
    需要严格规则时直接使用独立 `Ini`。
- 后端工厂: `infra/serde/SerdeEngineFactory.hpp` 的 `getEngineFor(ESerdeBackend::Ini)`。
