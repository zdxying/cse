# CSE 优化工具架构文档

## 概述

CSE（Common Subexpression Elimination）是一个独立的 C++17 优化器。它读取带
`//@cse` 标记（也识别 `// @cse`）的 C++ 源码，把每个标记区域独立地走一遍
「Frontend → IR → Passes → Backend」，写出 `<input>.cse`。核心是构建期 DAG 去重与跨语句 CSE；FreeLB 的
latset 解析、计数器传播与 `.ur.h` 生成位于 `plugins/freelb/`。`src/` 不依赖
`plugins/`；项目行为经 `CSEConfig` 钩子与两个 pass 槽位注入。

## 架构

### 四段流水线

```
源代码 → Frontend (Lexer/Parser/AST) → IR (DAG + 结构化语句) → Passes → Backend (CodeGen)
```

- **Frontend**：词法与语法分析，产出 AST；`//@cse` 区域由 `RegionExtractor`
  先行切出。
- **IR**：AST → DAG 节点图（构建期自然去重）+ 结构化语句树；节点身份、作用域、
  读写/别名模型都在这一层固定下来。
- **Passes**：按 `PassManager::createDefault` 的顺序改写 IR，见「优化管线」。
- **Backend**：`CodeGen` 把 IR 打印回 C++；括号、token 边界与 `for` 初始化子句
  由 IR 与发射器保证。

每个区域独立处理：一个区域读不了不影响其余，解析失败的区域原样透传。

### 目录结构

```
src/
├── frontend/
│   ├── token.h           # Token 类型定义
│   ├── lexer.h/cpp       # 词法分析器（支持 CSEConfig）
│   ├── ast.h             # AST 节点定义
│   ├── parser.h/cpp      # 递归下降解析器（支持 CSEConfig）
│   ├── region_extractor.h/cpp  # //@cse 区域提取
│   ├── diagnostics.h     # Diagnostic / CSEError
│   └── cse_config.h      # CSEConfig 配置结构
├── ir/
│   ├── dag_node.h        # DAG 节点定义与身份字段表
│   ├── opcode.h/cpp      # 运算符分类（OpCode）+ 拼写/优先级表
│   ├── statement.h       # IR 语句定义
│   ├── ir_module.h/cpp   # IR 模块（节点池 + 去重）
│   ├── ir_builder.h/cpp  # AST → IR 转换
│   ├── location.h/cpp    # 读写/别名模型（Location / LocationSet / AliasInfo）
│   ├── stmt_walk.h       # 语句拥有的表达式槽位遍历
│   ├── ir_printer.h/cpp  # 确定性 IR 结构打印（--dump-ir）
│   └── ir_utils.h        # 替换、折叠、按种类重建
├── passes/
│   ├── pass.h            # Pass 基类（效果保持 / 跨语句声明）
│   ├── pass_manager.h/cpp # Pass 管理器
│   ├── loop_unroll.h/cpp # 计数循环展开
│   ├── constant_fold.h/cpp    # 常量折叠
│   ├── algebraic_simplify.h/cpp # 代数简化 + 强度削减 + 常量乘法合并
│   ├── reassociate.h/cpp # 加法重结合（按全局频率排序，形成共享前缀）
│   ├── cse_pass.h/cpp    # 跨语句 CSE
│   ├── expr_recomb.h/cpp # 表达式重组
│   ├── value_prop.h/cpp  # 值传播
│   ├── dce.h/cpp         # 死代码消除
│   └── cleanup.h/cpp     # 末段清扫
├── backend/
│   └── codegen.h/cpp     # 代码生成
└── analysis/
    ├── cost_model.h/cpp  # FLOP 成本分析
    ├── effects.h/cpp     # 可观察效果签名（效果保持 oracle 与 --verify 用）
    └── purity.h          # 纯度、可投机性、可共享值的唯一判定处
plugins/
└── freelb/
    ├── cuda_skip.h/cpp   # __xx__ token 过滤器
    ├── config.h          # FreeLB 配置工厂函数
    ├── lattice_resolve.h/cpp  # latset::c/w 常量解析（硬编码查找表）
    ├── counter_prop.h/cpp # 计数器传播（FreeLB 注入的 post-algebra pass）
    ├── cse_main.cpp      # bin/cse 入口
    ├── ur_emit.h/cpp     # .ur.h 生成器核心（generateUrHeader）
    └── ur_emit_main.cpp  # bin/csegen 入口
```

## 配置系统

`CSEConfig`（`src/frontend/cse_config.h`）是前端与 pass 的唯一配置入口；默认值
保守，适用于通用 C++。

| 类别 | 字段 | 作用 |
|------|------|------|
| 前端 | `tokenFilter` / `simplifyBraceInit` | 跳过指定 token；`T{expr}` 是否简化为 `expr` |
| 数值授权 | `assumeNumericCommutative` / `assumeNumericAssociative` / `allowFpReassoc` / `allowUnsafeFpIdentities` / `assumeFloatOperands` | 交换/结合律、浮点重排、特殊值恒等式、无法证明 kind 时的浮点假设；见「代数规则的授权档」 |
| 纯度/别名 | `isPureFunction` / `noAlias` | 注册项目纯函数；声明不同指针/引用参数互不别名 |
| 项目钩子 | `resolveName` / `lowerVectors` / `vectorDim` / `isVectorType` / `isVectorProducingCall` / `vectorLocalName` / `constBindings` | 名字→常量、向量降级、非类型模板实参等通用扩展点 |
| 成本钩子 | `vectorLanes` / `callResultLanes` / `callCost` / `binaryOpCost` | `CostResult` 口径的 lane 与调用成本 |

```cpp
// 通用 C++ 模式（保守）：不假设交换/结合，未知调用视为有副作用
cse::CSEConfig config;

// FreeLB/CUDA 模式（激进）：开启数值代数规则，注册 lattice 访问器为纯函数，
// 并注入 resolveName / 向量降级等钩子
cse::CSEConfig config = cse::freelb::createFreeLBConfig();
```

### CLI 与结果契约

- 默认使用 FreeLB 配置。`-s/--safe` 切换到保守语义：保留 token 过滤，关闭全部
  数值假设/授权档，同时关闭 `simplifyBraceInit` 与纯函数注册。
- `-r/--recombine` 请求 `ExprRecombine`，并单独授予它所需的
  `allowFpReassoc`；`-s -r` 仍会做因子提取，其它假设档保持关闭。
- 每个 `//@cse` 区域独立走完整流水线。解析失败的区域**原样输出**，诊断按
  `文件:行:列` 报出（行号是文件中的行，不是区域内的偏移）。
- 退出码：`0` = 所有标记区域都可读（不可支持但合法的区域是 no-op，不影响退出码），
  `2` = 部分区域读不了、原样透传，`3` = 全部区域都读不了，`4` = 内部不变量被破坏或
  `--verify` 失败。
- 找到标记区域后，输出文件在最后统一写出：正常返回（0/2/3/4）就一定有输出文件。
  这把「出错」与「崩溃」区分开。
- `--verify` 让 `PassManager` 在每个 pass 前后比较可观察效果；违反效果保持时退出 4。

FreeLB 特定逻辑位于 `plugins/freelb/`：

- `cuda_skip.h/cpp` — 过滤 `__any__`、`__host__`、`__device__` 等 CUDA 注解。
- `config.h` — FreeLB 默认配置工厂与纯函数注册。
- `lattice_resolve.h/cpp` — lattice 常量/点积解析。
- `counter_prop.h/cpp` — 注入在代数简化之后的计数器传播。
- `ur_emit.h/cpp` + `ur_emit_main.cpp` — `.ur.h` 生成（`bin/csegen`）。
- `cse_main.cpp` — 通用 CLI 驱动（`bin/cse`）。

## 正确性与安全模型

通用模式下优化保持可观察效果。以下规则由 `analysis/purity.h`、`ir/location.h`、
`IRBuilder` 与 `LoopUnroll` 共同执行；`--verify` 额外用 `analysis/effects.h` 在
每个 pass 前后比较效果的 multiset 与相对顺序（只有 `LoopUnroll` 允许复制效果）。

1. **效果/纯度**：调用默认视为有副作用，`createCall` 仅对纯调用去重；不纯调用
   不被合并、不跨语句提取。内建数学函数白名单只匹配**限定名**的自由调用
   （`std::sqrt`）：裸名 `max`/`min`/`abs` 可能解析到用户定义的同名函数，同名
   **成员**调用（`s.sin(x)`）同样不查白名单；其余情况只有 `isPureFunction` 判真
   才算纯。任何把两处出现折叠为一处的重写，都要求被折叠的子表达式**深纯**
   （`isReferentiallyTransparent` 递归检查全部操作数）：`DAGNode::pure` 是逐节点
   的，`createBinaryOp` 一律置 `true`，所以 `f() + 1.0` 顶层 `pure` 为真而内部
   调用不纯，浅检查会把它当作可重复求值。丢弃一个值是否安全由
   `hasObservableEffect` 回答（不纯调用、`++`/`--`、表达式形式的赋值）。

2. **内存与别名**：IRBuilder 预扫描得到只读根（已声明、未被写、未传入调用，且
   不是未获 `noAlias` 许可的指针/引用根）；仅只读根的 load 可去重/跨语句共享，
   可变/未知根的 load 每次独立，避免跨 store 复用。`const T*` / `const T&` 参数
   **不**自动视为只读根：`const` 只承诺"不通过它写"，不承诺没有别的指针/引用指向
   同一块内存（`f(x, x)` 会让 `f(const V& v, V& w)` 的 `v`、`w` 指向同一对象）；
   `*`、`&`、`[` 形参一视同仁，都靠 `noAlias` 才恢复共享。

3. **赋值可见性**：变量按名字驻留，重新赋值不产生新节点，所以跨语句的改写必须显式
   检查写操作——`CSEPass` 只把子表达式提到"定义点到每个使用点之间都没有写入其
   操作数"的位置，`ValueProp` 只内联初值依赖的变量在整个函数内都没被写过的定义。
   这与第 2 条是同一条原则，只是作用于**标量变量**而非内存访问。元素/成员写也算
   写：`a[i] = x;` / `p->f = x;` 写的是 lvalue 的**根**变量，各处扫描通过
   `ir/location.h` 的 `collectWriteLocations` 记录；只记根是刻意的，因为可能有
   别的指针指向同一对象。`VarDecl` 不记入（它引入新名字，不是覆盖已有值）。这类
   写在 IR 里是结构化语句 `AssignIR`（`targetExpr` + `value`），与 `lowerVectors`
   无关。

4. **支配安全与可投机性**：出现在 `if`/`else`/循环内的子表达式标记为 nested，
   不参与顶层提取，避免把条件执行的计算提升为无条件计算。表达式级守卫
   （`c ? x : y` 的 x/y、`a && b` / `a || b` 的右操作数）另按 `isSpeculatable`
   判定：
   `isReferentiallyTransparent` 回答"能不能重复/移动"，`isSpeculatable` 回答
   "能不能在源码不会求值的位置求值"，两者只在陷阱处分歧。于是
   `k ? a*b : 0` 里的 `a*b` 仍被共享，而 `k ? 1/k : 0` 里的除法不会；把整个
   三元/`&&` 提取是安全的（它保留内部短路）。

5. **作用域与生成名**：IRBuilder 维护作用域栈并对遮蔽变量 alpha-rename
   （如 `y__s1`），判据覆盖仍在作用域内的同名、已 pop 的兄弟作用域的同名，以及
   region 读或写但不声明的外部名字。所有 invent 出来的名字（builder `*__s*`、展开
   `*__u*`、CSE `_cse_*`）都经 `IRModule::freshName` 分配，保证不等于任何已驻留
   变量名或已预留名。

6. **循环展开前提**：仅当下列条件全部满足才展开，否则保留循环：

   - 循环体内每个局部声明都能被内联消除（初始化表达式为纯、非变量、不被重新赋值）
     或安全改名；这避免把同一局部变量跨迭代共享、进而被误当作循环不变量。
   - 循环变量未被循环体改写（`i++`），否则常量替换会得到 `0++`。
   - 循环变量不被循环语句之外读，否则展开删掉它的声明会让那个读悬空。
   - 循环体不含属于本循环的 `break`/`continue`，否则展开会把它们移出所命名的循环。

   「被改写」的判据是 `collectWriteLocations`，覆盖 `++`/`--` 与以表达式形式书写
   的赋值；「循环外被读」的判据是整函数读集减去本循环语句（body + cond + update）
   的读集。

7. **代码发射**：IR 树的右子节点在同等优先级下一律保留括号，因为浮点下
   `a - (b - c)`、`a / (b * c)` 甚至 `a * (b * c)` 与去掉括号后的形式都不等价。
   前缀 `-`/`+` 的操作数若自身以同号开头（如折叠出的常量 `-5`）必须加括号，否则
   `-(-5)` 会打成 `--5`；`for` 的初始化子句无论是声明还是赋值都要发射，否则
   `for (i = 0; …)` 会退化成 `for (; …)`。

代数规则默认关闭，按**授权类型**分成四档：

| 开关 | 授权 |
|------|------|
| `assumeNumericCommutative` / `assumeNumericAssociative`（联合生效） | 交换/结合律重排（叶子排序、乘法链常量合并）。**精确恒等式不受此门控** |
| `allowFpReassoc` | 浮点加法重排（`Reassociate`）、乘法链重排（`normalizeProduct`）、公因子提取（`ExprRecombine`；`-r` 单独授予该项） |
| `allowUnsafeFpIdentities` | `x*0→0`、`x-x→0`、`0/x→0`、`x/x→1`、`x+0→x`、`0-x→-x`（在 `0`/`±inf`/`NaN`/符号零处改变结果）；以及 `Cleanup` 的加法链合并（`x=0; x=x+a; x=x+b → x=a+b`，丢弃累加器前导 `0`） |
| `assumeFloatOperands` | 数值 kind 无法证明的操作数（load、调用、模板相关成员）按浮点处理。影响零恒等式替换常量的 kind，以及删除浮点常量（`1.0`/`0.0`）时的 kind 判定；可证明的整型始终优先。FreeLB profile 开启，`-s` 关闭 |

精确档（逐位精确的恒等式，见「代数简化」）在任何档位都运行，包括 `-s`。默认档
（FreeLB profile）打开全部假设/授权档；`-s` 关闭它们，并关闭 brace-init 简化与
纯函数注册。

这四条零恒等式（`x*0`、`x-x`、`0/x`、`x/x`）替换的是**整个表达式**，所以替换出
的常量必须携带表达式的 int/float kind——对 double 发射整型 `0` 会把外层 `/ 4`
变成整除。kind 由 `joinNumericKind` 推断，而 load 的 kind 不可证（`Unknown`），
于是授权由 `assumeFloatOperands` 单独给出：开启后 `0 * cell[i]` 折成 `0.0`。
**可证明的整型始终优先**：`(0 * k + 2) / 4` 在开启时仍是整数除法。一个有**浮点
字面量**的零乘积（`0.0 * a[i]`）不需要任何授权，按 usual arithmetic conversions
即可证明。

## 优化管线

默认管线按以下顺序执行（方括号为可选/受配置门控的环节）：

```
LoopUnroll → [Resolve] → ConstantFold → AlgebraicSimplify → [PostAlgebra]
  → [Reassociate] → CSEPass → [ExprRecombine → AlgebraicSimplify] → ValueProp → DCE → Cleanup
```

| Pass | 功能 |
|------|------|
| **LoopUnroll** | `for (i=0; i<N; ++i)` 展开为 N 条语句，并内联/改名循环体局部变量 |
| **Resolve** | 插件 Pass（`resolvePass`）：解析 `latset::c/w` 为常量/点积（FreeLB） |
| **ConstantFold** | 编译期计算常量表达式 |
| **AlgebraicSimplify** | 精确恒等式始终运行；交换律排序与常量乘法合并受 `numeric_`/`allowFpReassoc` 门控；特殊值恒等式另有 `allowUnsafeFpIdentities` |
| **PostAlgebra** | 插件 Pass 槽位（`postAlgebraPass`），FreeLB 注入 **CounterProp**：直线计数器解析（`tensor[i]→tensor[0]`）、降级向量局部索引（`unew[1]→unew_1`）、常量条件折叠 |
| **Reassociate** | 加法项按全局出现频率重排，使对称语句共享不变前缀（需 `assumeNumericAssociative` **且** `allowFpReassoc`） |
| **CSEPass** | 跨语句公共子表达式提取（含干扰写检查，见安全模型第 3 条） |
| **ExprRecombine** | 乘法因子提取（`-r` 启用；除法与不纯节点不参与） |
| **ValueProp** | 内联简单初值到使用处（初值依赖的变量须在整个函数内未被写过） |
| **DCE** | 删除死声明（未初始化/零初始化且未被读写）与死存储（私有存储、无副作用右值） |
| **Cleanup** | 末段清扫：删除没被读也没被写的零初始化/常量初始化声明、自赋值；合并链式赋值 |

### 插件注入

```cpp
static PassManager createDefault(const CSEConfig& config,
                                 bool enableRecombine = false,
                                 std::unique_ptr<Pass> resolvePass = nullptr,
                                 std::unique_ptr<Pass> postAlgebraPass = nullptr);
```

两个可选的项目专用 Pass 槽位：`resolvePass` 在循环展开之后、通用代数/CSE 之前
运行；`postAlgebraPass` 在代数简化之后、`Reassociate`/`CSEPass` 之前运行。
FreeLB 分别通过 `plugins/freelb/lattice_resolve` 与 `counter_prop` 提供，
核心 `src/` 不依赖任何项目。

## Pass 行为

### DAG 自然 CSE 与 IR 身份

IR 构建期即完成自然去重：节点由按种类划分的工厂创建并 `intern`，结构相同且
`pure` 的节点复用；`createNode` 与 `intern` 都是 private，节点只能经由工厂产生。

身份字段表只写一处：`CSE_DAGNODE_IDENTITY_FIELDS`（`src/ir/dag_node.h`）列出决定
"两个节点是否同一节点"的全部字段，`DAGNode::sameContentAs()` 与
`computeHash()` 都展开这个宏——被比较的集合与被哈希的集合按构造相同。`pure` 也在
身份字段中；`numText` / `symbol` 刻意不在（同一个常量节点只有一份拼写）。
`IRModule::verify()` 在每个 pass 之后检查不变量（debug 构建下断言）；
`tests/verify/verify_node_identity.cpp` 从外部钉住这张表。

**局限**：只识别结构完全相同的表达式，不识别代数等价（如 `(a*b)*c` ≠
`a*(b*c)`）。

### 跨语句 CSE（CSEPass）

候选是至少出现在两条顶层语句中的非叶节点。语句级 nested 节点整体排除；表达式级
守卫节点只在该节点 `isSpeculatable` 时提取。每个候选在提出前检查定义点到每个使用点
之间没有写入其操作数（逐语句 `AliasInfo::writtenBy`），并把引用别名闭包展开到
referent。每轮只提取一个候选，然后重新扫描；迭代上界是非叶节点数，因此不会截断
独立候选。

### 结构化控制流

IR 使用嵌套结构表示控制流，而非 CFG：

```
ForLoopIR
├── init: StmtIR
├── cond: DAGNode*
├── update: DAGNode*
└── body: StmtIR (通常是 BlockIR)
```

### 常量折叠（ConstantFoldPass）

BinaryOp 的两个操作数都是 Constant 时在编译期计算结果（`foldConst`）。

**例外：除数为 0 时不折叠。** 整数除零是未定义行为、浮点除零是 inf/NaN，`0` 是
唯一对两者都错的答案，而 IR 不携带类型、无法区分二者；表达式原样保留。

**常量文本保留字面量拼写，int/float 类型参与身份。** `createConst` 只在调用方未
给出拼写时才自行格式化：整数值的浮点（`isFloat`）补 `.0`，整数值的整数发射为裸
数字；源码里的 `1e16` / `0.5` / `-0.0` 按原样发射。常量的 int/float 类型
（`isFloat`）是节点身份的一部分——`4/3` 与 `4.0/3` 是不同节点、不同值，折叠与
去重都必须保留这一位；`numText`（拼写）不参与身份。codegen 对下标位置仍强制发射
整数字面量。

### 代数简化（AlgebraicSimplifyPass）

规则分两档：**精确档**始终运行（每条都逐位精确，含 `±0.0`/`±inf`/`NaN`/次正规
数，故 `-s` 与裸 `CSEConfig` 也保留），**假设档**按开关门控。

**精确档（无需任何假设）：**

- `a * 1 → a`，`1 * a → a`
- `a - 0 → a`（两个符号零都精确），`a / 1 → a`
- `--a → a`（双重否定消除；不碰 `--x` 前缀自减）
- `a + (-b) → a - b`，`a - (-b) → a + b`
- `x * 2 → x + x`（操作数须深纯 `isReferentiallyTransparent`，否则会重复求值）
- `(-a) * (-a) → a * a`
- `a * (-b) → -(a * b)` 等符号规范化。唯一例外：`0 * inf` / `inf * 0` 产生的
  NaN 符号位与源码不同；NaN 的符号/payload 不是 IEEE-754 的可观察值，`isnan` 与
  所有比较都不区分，故仍留在精确档。`verify_exact_identities.cpp` 同时钉住两半：
  非 NaN 操作数逐位精确，NaN 仍是 NaN。

**假设档（需 `numeric_` = 交换律 ∧ 结合律）：**

- 交换律排序：叶子节点按常量优先、变量名字典序排列，促进后续 CSE 匹配
- 乘法链重排：常量因子合并 + 左结合重建（另需 `allowFpReassoc`）

**另需 `allowUnsafeFpIdentities`（在 `0`/`±inf`/`NaN`/符号零处改变结果）：**

- `a + 0 → a`，`0 + a → a`（`a = -0.0` 时和为 `+0.0`）
- `0 - a → -a`（`a = +0.0` 时差为 `+0.0`，而 `-a` 是 `-0.0`）
- `a * 0 → 0`，`0 * a → 0`
- `0 / a → 0`
- `a - a → 0`，`a / a → 1`

注：`x + 0` 因符号零**不精确**，而 `x - 0` 精确——两者不可混为一谈。常量匹配要求
`symbol.empty()`：符号常量（如 `ns::w<T>(1)`，为发射保留）带真实数值但**不是**
数值字面量，不能被折掉（与 `normalizeProduct` 的常量折叠守卫一致）。

### 表达式重组（ExprRecombinePass）

分配律方向的公因子提取（`-r` 启用），只对乘法生效：

- `a * x + a * y → a * (x + y)`
- `a * x + b * x → (a + b) * x`
- `a * x - b * x → (a - b) * x`
- `a * x + a → a * (1 + x)`，`a * x - a → a * (x - 1)`

四种左右组合都会尝试。两条硬约束：

- **除法永不参与**。IR 不携带类型信息，`a / x + b / x` 只在精确除法下等于
  `(a + b) / x`；对整数 `3/2 + 1/2 == 1` 而 `(3+1)/2 == 2`，所以既不能输出 `/`
  形式，更不能输出 `*`。
- **公因子必须通过 `samePureExpr`**（`a->pure && b->pure && sameExpr(a, b)`）。
  该重写把两处文本出现折叠为一处，所以带副作用的调用、可写位置的 load 都不能作
  公因子；结构相同并不意味着可以互换。

跨组合（如 `la*lb + ra*la → la*(lb+ra)`）与 `a*x ± a` 的形态归一需要 `+`/`*`
满足交换律，因此挂在 `assumeNumericCommutative` 上；直组合（`la==ra`、`lb==rb`）
无条件生效。

### 值传播（ValuePropPass）

内联简单初值到使用处，消除中间变量：

```cpp
double val = a;       // 初值是 Variable → 可内联
double t = val * val; // 内联后: double t = a * a;
```

`double val = a * x;` **不会**被内联——`isCheapShareableValue`
（`analysis/purity.h`）只接受 Constant / Variable / MemberAccess / ArrowAccess
（后者基须为 Variable），任何 `BinaryOp` 都会被拒绝。

安全约束：

- 只内联上述"零成本"表达式；
- 被内联的变量本身不得被重新赋值（预扫描 `collectWriteLocations` 得到
  `reassigned`，再经 `AliasInfo::closeWrites` 把引用局部变量的写展开到其 referent）；
- 不得内联某个引用的 referent（`AliasInfo::isAliasedReferent`）：`double& r = v;`
  的初值是 lvalue 绑定上下文，把 `v` 换成它的值会得到 `double& r = 0.5;`；
- 初值读取的变量不得在整个函数内被写过（`initDepsStable` 经
  `AliasInfo::writeAffectsRead` 查同一 `reassigned`），否则
  `double t = a; a = b; return t;` 会被改写成 `a = b; return a;`；
- 不内联函数参数本身——但"以只读参数为初值的局部"可以内联；
- 只处理 `VarDecl` 初值；`Assign` 的右值不内联。

### 死代码消除与末段清扫（DCEPass / CleanupPass）

DCE 删除两类死代码，迭代至不动点（每轮重算读集与写集）：

- 未初始化或零初始化的局部声明，且既没被读也没被写；
- 目标是本函数私有存储、读计数为 0、右值无副作用的赋值。

**赋值的三条独立不删除条件：**

| 条件 | 反例 |
|------|------|
| 复杂 lvalue（`a[i] = v;`、`p->f = v;`）写的是内存，lvalue 不是一个可推理的名字 | —— |
| 目标不是本函数**私有存储**（值形参、本模块的 `VarDecl`；引用形参/引用局部不算，指针算） | `g = a;` 对全局的写不能删；`void setv(double& v) { v = 1.0; }` 写的是调用者对象 |
| 右值本身有副作用（`hasObservableEffect`：不纯调用 / `++` `--` / 表达式形式的赋值） | `unused = y++;` 删除后 `y` 的增量消失 |

`reads` 与 `written` 每轮重算，所以"删除赋值 → 下一轮释放声明"会自行收敛。DCE
递归进入 `if`/`for` 嵌套块。

末段 `CleanupPass` 做清扫：删除没被读**也没被写**的零初始化或常量初始化声明
（与 DCE 共用 written 判定）、删除自赋值（`u_value[i] = u_value[i];`）、合并链式
的声明赋值与赋值语句。`pruneBlock` 递归进入 `if`/`for` 体与嵌套 `{ }`
（`pruneNested`），每层作用域独立清理。

**链式合并受 `allowUnsafeFpIdentities` 门控**：`x = 0; x = x + a; x = x + b`
合并为 `x = a + b` 时丢掉了累加器的前导 `0`，这正是 `x + 0 → x` 那条符号零恒等
——`a = b = -0.0` 时原式得 `+0.0`、合并式得 `-0.0`（`1.0/x` 因此从 `+inf` 变
`-inf`）。默认档合并，`-s` 档保留原链。

### 循环展开（LoopUnrollPass）

将 `for (i = 0; i < N; ++i)`（N 为字面常量）展开为 N 条语句：克隆循环体、用常量
替换循环变量、把结果直接拼接进父 Block（保证跨语句 CSE 可见）。循环体局部若仅在
该循环体内使用则内联并删除声明；若被改写（`t++;`）则改名（`t__u0` …）而非内联，
否则内联会把 `t` 代进它自己的 `++`。展开前提见「正确性与安全模型」第 6 条。

### 加法重结合（ReassociatePass）

将 `+`/`-` 链展平为带符号项，按每个带符号项在函数中的出现频率降序重建。出现频率
高的项排在前面，使对称语句（如 D3Q19 的 ± 方向对）形成相同的不变前缀，随后由 CSE
提取。带可观察效果的项不越过对它所写状态的读（`analysis/effects.h` 的效果顺序
oracle 也钉住这一点）。

### 前端 C++ 子集

前端是面向内核子集的递归下降解析器。它支持命名空间/`using`/`typename`/`class`
模板前缀、`static`/`const`/`inline` 修饰、引用与指针形参、参数/局部声明中的
`unsigned int` 复合拼写、`T{expr}` 括号初始化（`simplifyBraceInit` 控制是否简化）、
前后缀 `++`/`--`、一元 `+`、科学计数法与浮点后缀、`f(void)` 空形参表、
`<<`/`>>` 与位算子，以及 `latset::c<LatSet>(k)` 这类不透明模板调用。
模板实参与 `<` 比较的区分规则：`ident<…>` 仅在实参列表合法、且 `>` 后紧跟
`(`/`::`/`.`/`->`/`[` 时才算模板 id，否则按 `<` 比较解析。

不支持的构造：字符串/字符字面量（区域会被跳过并原样输出）；预处理指令不建模
——词法器发出 `PreprocessorDirective` 词法单元，区域提取器据此置
`CSERegion::hasDirectives`，驱动把该区域原样透传并计入 `skipped-unsupported`。
`#` 在注释/字符串里是普通文本。夹具级支持面与缺口见 `tests/README.md`。

### IR 工具函数

- **`stmt_walk.h`**：`forEachExpr(stmt, visit, includeLvalue=true)` 遍历**语句拥有
  的表达式槽位**；`forEachExprDeep` 含嵌套语句。新增语句种类或槽位只改这里。
  `includeLvalue=false` 只给 `CSEPass` 用：CSE 不能把 lvalue 换成临时变量，只替换
  索引里的子表达式。
- **`location.h`**：`locationRoot`/`locationOf`、`collectReadNames`/
  `countReadNames`、`collectWriteLocations`、`LocationSet`/`mayAlias`/`AliasInfo`。
  `AliasInfo::writtenBy` 给出一条语句的写集，`isAliasedReferent` 判断某名是否为
  某引用的 referent，`writeAffectsRead` 是写集闭包的镜像。
- **`ir_utils.h`**：`substitute`、`foldConst`、`rebuildWithOperands`（按种类重建，
  保留 `++`/`--` 拼写与 cast/conditional）。
- **`analysis/purity.h`**：`hasObservableEffect`、`isReferentiallyTransparent`、
  `isSpeculatable`、`isEffectFreeLvalue`、`isCheapShareableValue`——纯度问题的
  唯一判定处，不要在 pass 里写局部变体。

## 已知局限

| 局限 | 说明 | 影响 |
|------|------|------|
| **结合律仅限加法** | 乘法链只在常量因子层面重排 | 一般结合律仍不识别 |
| **类型系统不完整** | 类型用字符串表示 | 不支持类型检查、模板实例化、类型推导 |
| **区域内无错误恢复** | 区域解析失败时整块跳过、原样输出，退出码区分全部成功 / 部分跳过 / 全失败 | 一个区域内只报第一个错误；跨区域已能继续 |
| **仅处理标记区域** | 只解析 `//@cse` 标记的代码 | 无法跨区域优化 |
| **作用域只覆盖本区域** | alpha-rename 处理本函数内的遮蔽，以及本区域读过/写过的外部名字；但工具看不到 region 之外的声明 | 若 region 外有一个与 region 内局部同名、且 region 从未读写的名字，两者仍可能共用节点；影响面很窄，彻底修法需要扫描 region 外的顶层声明 |
| **数组复合赋值未展开** | `a[i] += x` 保留为 `AssignIR::compoundOp`，未展开为 `a[i] = a[i] + x` | 需要显式读写 lvalue 的重写不会作用于它；求值次数不变 |
| **无通用 constexpr** | 仅模式匹配已知 lattice | 其他 constexpr 调用仍不透明 |
| **写集合只看 lvalue 的根** | `collectWriteLocations` 对元素/成员写只记根变量名 | 若两个不同根实际别名同一对象，检查会认为"没写"；`noAlias` 与"根名不同即不别名"是当前的全部依据 |
| **写集合不含声明** | `VarDecl` 不计入写集合 | 名字遮蔽已做 alpha-rename，故不会与旧绑定混淆；新增读写分析时注意这一点 |
| **break/continue 阻断展开** | 循环体内出现属于本循环的 `break`/`continue` 时不展开该循环 | 该循环体不参与跨语句 CSE（正确性优先）；`break;`/`continue;` 以「名为 `break`/`continue` 的裸变量表达式语句」形式到达 IR |
| **FLOP 口径是「按引用次数」** | `cost_model` 按每个使用点（引用次数）计费，而非 DAG 唯一节点数；物化临时只算其定义一次（`totalNodes` 仍为唯一节点数） | 近似生成代码的开销：DAG 已哈希去重，而 codegen 在每个使用点重印未物化的节点 |
| **常量文本不进身份** | `numText` / `symbol` 不是节点身份的一部分 | 同一个常量节点只有一份拼写，可能被算术位置与下标位置共用；下标由 codegen 保证整型 |
| **`if`/`else` 体内不做跨语句 CSE** | `nested` 是一个布尔标记：节点只要在条件执行里出现过就整体排除 | 过度保守而非不安全：`if (n>0) { t=a*b; u=a*b; }` 完全不提取；改成支配树即可放开（暂不实施，重启条件见本节末） |
| **CSE 提取是 O(n²)** | 每提取一个候选就重扫全部语句、重算写集 | 候选多时明显变慢；单遍化可独立进行，判据是 FLOP 与数值校验不变（暂不实施，见本节末） |
| **解析深度上限 512** | 表达式嵌套超过 512 层报 `CSE-PARSE-DEPTH` 并透传区域 | 防的是栈溢出崩溃；真实内核远低于此 |

以下改造暂不实施：B1 支配树（放开 `if` 体内 CSE）、C1 CSE 单遍化
（O(n²)→O(n)）、C2 变量版本化（消除按名驻留 + 写集补偿层）、E1 region 外声明
扫描、E2 `AssignIR` 双表示统一、P2-7 `IRBuilder::buildExpr`/`buildValue` 双路径
合并。它们要么提升能力、要么只带来整洁收益，都没有正确性增量，而回归面集中在核心
pass。重启条件：B1 需要真实内核因 `if` 体内 CSE 缺失而收益可见；C2 需要「忘记查
写集」类缺陷再现成模式；E1 需要当前的读/写外部名登记再暴露新缺口；P2-7 需要两条
镜像路径出现行为漂移（目前靠孪生测试兜住，合并必须带向量开启档逐位不变的验证）。
C1 是纯性能项，随时可做，判据是单遍化后 FLOP 与数值校验不变。

## 未来优化方向

### 高优先级

| 方向 | 描述 | 状态 |
|------|------|------|
| **LICM** | 保留循环形式时的循环不变量外提 | 待实现 |

### 中优先级

| 方向 | 描述 | 状态 |
|------|------|------|
| **通用 constexpr 求值** | 解析 constexpr 数组/函数，不依赖硬编码表 | 待实现 |
| **乘法一般结合律** | `(a*b)*c` 与 `a*(b*c)` 视为等价 | 部分实现（常量因子） |
| **CounterProp 失效细化** | `updateKnown`/`entrySeedFor` 遇到任何 wildcard 写集就清空整个计数器表；可细化为只失效「实际被写」的计数器 | 待实现（性能优化） |

### 低优先级

| 方向 | 描述 | 状态 |
|------|------|------|
| **错误恢复** | 遇到 `;` 或 `}` 时同步，继续解析 | 待实现 |
| **CFG 支持** | 支持 break/continue/goto | 待实现 |
| **效果签名覆盖全部调用** | `analysis/effects.h` 只统计不纯调用，所以“把有副作用的调用误判为纯”在 `--verify` 下不可见；同时记录纯调用可让这类误判以契约违反浮现 | 待实现 |

## 成本模型与实测

成本模型按**标量 FLOP、向量按 lane 加权**计算：向量算子的代价为其 lane 数，
向量点积为 `2d-1`，纯 helper/intrinsic 调用由 FreeLB 插件按名给定代价
（`getnorm2 -> 2d-1` 等），未知调用与无法解析的循环会被显式计数
（`unmodeledCalls` / `unknownLoops`）。子表达式按**引用次数**计费：每个使用点
一次，物化临时只算其定义一次（`totalNodes` 仍是唯一节点数）；循环体按执行次数
缩放（三角 `for (b=a; b<d; ++b)` 嵌套精确计数）。规则与实现见
`src/analysis/cost_model.h` 与 `docs/cost_model_redesign.md`。

### D3Q19 equilibrium 夹具

`./bin/cse tests/fixtures/equilibrium_d3q19.cpp -c`：

| 版本 | FLOPs |
|------|:-----:|
| 原始循环版（Before） | 323 |
| **CSE 工具输出（After）** | **84** |

工具输出相对原始循环版节省 239 flops（74.0%）。

### FreeLB 真实头文件

`csegen --cost` 与生成 `.ur.h` 使用同一条管线（per-struct `lowerVectors`，以及随之
注入的 `CounterPropPass`；不启用 `ExprRecombine`），因此数值与生成代码一致：

```
./bin/csegen --cost --lattice D2Q9 --lattice D3Q19 \
    ~/FreeLB/src/lbm/<name>.h /tmp/<name>.ur.h
```

| 头文件 | lattice | Before | After | Saved | vector-ops |
|--------|:-------:|:------:|:-----:|:-----:|:----------:|
| `moment.h` | D2Q9 | 956 | 355 | 62.9% | 0→0 |
| `moment.h` | D3Q19 | 3113 | 781 | 74.9% | 0→0 |
| `equilibrium.h` | D2Q9 | 138 | 45 | 67.4% | 9→0 |
| `equilibrium.h` | D3Q19 | 328 | 91 | 72.3% | 19→0 |
| `force.h` | D2Q9 | 396 | 150 | 62.1% | 0→0 |
| `force.h` | D3Q19 | 1482 | 353 | 76.2% | 0→0 |

## 构建

仓库产出静态库、动态库与两个可执行文件：

```
bin/
├── libcse.a      # 静态库
├── libcse.so     # 动态库
├── cse           # 通用 CSE CLI（静态链接 libcse.a）
└── csegen        # FreeLB .ur.h 生成器（静态链接 libcse.a）
```

两个入口分别在 `plugins/freelb/cse_main.cpp` 与 `plugins/freelb/ur_emit_main.cpp`；
库目标由 `src/` 与 `plugins/freelb/` 下除这两个入口外的全部 `.cpp` 组成。

```bash
make          # 构建静态库、动态库与 bin/cse、bin/csegen
make test     # 自动构建后运行回归（tests/run_tests.sh）
make sanitize # ASan/UBSan 下重建并跑同一脚本
make release  # OPT=-O2 重新构建
make install PREFIX=/usr/local DESTDIR=  # 安装 cse/csegen 与 libcse.a/.so
make clean    # 清理

./bin/cse input.cpp -c         # 分析 FLOP 成本（--json 输出 JSON）
./bin/cse input.cpp -r         # 优化文件，启用表达式重组
./bin/cse input.cpp -s         # 保守模式（不启用不安全的代数规则）
./bin/cse input.cpp -v         # 打印每个 pass（stderr）
./bin/cse -h                   # 全部选项
./bin/csegen tests/csegen/moment.h /tmp/moment.ur.h   # 生成 .ur.h
```

`csegen <input.h> <output.h>` 的输入 basename 必须是 `moment` / `equilibrium` /
`force` 之一，它决定 include 路径与 namespace。`//@cse` 区域内的 `namespace` 会被
完整处理：其中的 `using`、结构体与函数都经过同一优化管线并原样包裹在
`namespace X { ... }` 中输出。

## 测试

夹具、校验器与阶段判据的权威清单见 `tests/README.md`；`make test` 运行
`tests/run_tests.sh`，`make sanitize` 在 ASan/UBSan 下重跑。
