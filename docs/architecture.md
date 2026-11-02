# CSE 优化工具架构文档

## 概述

CSE（Common Subexpression Elimination）优化工具是一个基于 LLVM 风格四段架构的 C++ 代码优化器。它分析标记了 `//@cse` 注释的代码区域，识别公共子表达式并进行优化。

## 架构

### 四段流水线

```
源代码 → Frontend (Lexer/Parser/AST) → IR (DAG-based) → Passes → Backend (CodeGen)
```

每一段独立运作，通过明确的数据结构连接：

- **Frontend**：词法分析 → 语法分析 → AST
- **IR**：AST → DAG 节点图（自然去重）→ 结构化语句树
- **Passes**：循环展开 → 常量折叠 → 代数简化 → 计数器传播 → 重结合 → CSEPass → 表达式重组 → 值传播 → 死代码消除
- **Backend**：优化后的 IR → 生成 C++ 代码

### 目录结构

```
src/
├── frontend/
│   ├── token.h           # Token 类型定义
│   ├── lexer.h/cpp       # 词法分析器（支持 CSEConfig）
│   ├── ast.h             # AST 节点定义
│   ├── parser.h/cpp      # 递归下降解析器（支持 CSEConfig）
│   ├── region_extractor.h/cpp  # //@cse 区域提取
│   └── cse_config.h      # CSEConfig 配置结构
├── ir/
│   ├── dag_node.h        # DAG 节点定义
│   ├── opcode.h/cpp      # 运算符分类（OpCode）+ 拼写/优先级表
│   ├── statement.h       # IR 语句定义
│   ├── ir_module.h/cpp   # IR 模块（节点池 + 去重）
│   ├── ir_builder.h/cpp  # AST → IR 转换
│   ├── location.h/cpp    # 读写/别名模型（Location / LocationSet / AliasInfo）
│   ├── stmt_walk.h       # 语句拥有的表达式槽位遍历（forEachExpr）
│   ├── ir_printer.h/cpp  # 确定性 IR 结构打印（--dump-ir）
│   └── ir_utils.h        # 工具函数（替换、折叠、按种类重建）
├── passes/
│   ├── pass.h            # Pass 基类
│   ├── pass_manager.h/cpp # Pass 管理器
│   ├── loop_unroll.h/cpp # 计数循环展开
│   ├── constant_fold.h/cpp    # 常量折叠
│   ├── algebraic_simplify.h/cpp # 代数简化 + 强度削减 + 常量乘法合并
│   ├── reassociate.h/cpp # 加法重结合（按全局频率排序，形成共享前缀）
│   ├── cse_pass.h/cpp    # 跨语句 CSE Pass
│   ├── expr_recomb.h/cpp # 表达式重组
│   ├── value_prop.h/cpp  # 值传播
│   └── dce.h/cpp         # 死代码消除
├── backend/
│   └── codegen.h/cpp     # 代码生成
└── analysis/
    ├── cost_model.h/cpp  # FLOP 成本分析
    └── effects.h/cpp     # 可观察效果签名（效果保持 oracle 与 --verify 用）
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

CSE 工具通过 `CSEConfig` 结构支持多种项目配置：

```cpp
struct CSEConfig {
  // 前端
  std::function<bool(const Token&)> tokenFilter;  // token 过滤器
  bool simplifyBraceInit = true;                  // T{expr} → expr

  // 语义/安全（默认保守，通用 C++ 安全）
  bool assumeNumericCommutative = false;          // 允许 +/* 交换律重排
  bool assumeNumericAssociative = false;          // 允许结合律/重结合
  bool allowFpReassoc = false;                    // 允许浮点重结合（含乘法链重排）
  bool allowUnsafeFpIdentities = false;           // 允许 x*0→0 / x-x→0 / 0/x→0 / x/x→1 /
                                                  //   x+0→x / 0-x→-x（0/±inf/NaN/符号零）
  bool noAlias = false;                           // 假设不同指针/引用参数互不别名
  bool assumeFloatOperands = false;               // kind 无法证明的操作数按浮点处理
                                                  // （只影响零恒等式替换常量的 kind）
  std::function<bool(const std::string&)> isPureFunction;  // 纯函数判定

  // 项目钩子（通用扩展点，`src/` 内无 FreeLB 语义）
  std::function<DAGNode*(IRModule&, const std::string&)> resolveName;  // 名字→常量
  bool lowerVectors;                              // 把向量类型降级为分量标量（只影响
                                                  // 向量代码，不改变普通语句的 IR 形状）
  int vectorDim;                                  // 向量维度（降级用）
  std::function<bool(const std::string&)> isVectorType;           // 类型判定
  std::function<bool(const std::string&)> isVectorProducingCall;  // 产生向量的调用
  std::function<std::string(const std::string&, long long)> vectorLocalName; // 向量局部命名
  std::unordered_map<std::string, double> constBindings;         // 非类型模板实参

  // 成本钩子（CostResult 口径的扩展点，见 docs/cost_model_redesign.md）
  std::function<int(const std::string& type)> vectorLanes;        // 类型的标量 lane 数
  std::function<int(const std::string& callee)> callResultLanes;  // 调用的结果 lane 数
  std::function<long long(const std::string&, const std::vector<int>&)>
      callCost;                                  // 纯调用的 FLOP 成本（负值=未建模）
  std::function<int(OpCode, int, int, long long&)> binaryOpCost; // 二元运算成本+结果 lane
};
```

### 使用方式

```cpp
// 通用 C++ 模式（保守）：不假设交换/结合，未知调用视为有副作用
cse::CSEConfig config;

// FreeLB/CUDA 模式（激进）：开启数值代数规则，注册 lattice 访问器为纯函数，
// 并注入 resolveName / 向量降级等钩子
cse::CSEConfig config = cse::freelb::createFreeLBConfig();

// `.ur.h` 生成器另按 latset 上下文补 lowerVectors / constBindings
```

CLI：默认使用 FreeLB 配置；`-s/--safe` 切换到保守语义（保留 token 过滤）。

每个 `//@cse` 区域独立走完整流水线，一个区域读不了不影响其余：解析失败的区域**原样输出**，
诊断按 `文件:行:列` 报出（行号是**文件**里的行，不是区域内的偏移）。退出码：0 = 全部成功，
2 = 部分区域被跳过，3 = 全部区域都失败。输出文件在最后统一写出，因此**只要进程正常返回，
就一定有输出文件**——这也是把"出错"与"崩溃"区分开的实际意义。

FreeLB 特定逻辑位于 `plugins/freelb/`：
- `cuda_skip.h/cpp` — 过滤 `__any__`、`__host__`、`__device__` 等 CUDA 注解
- `config.h` — FreeLB 默认配置工厂函数 + 纯函数注册
- `lattice_resolve.h/cpp` — lattice 常量/点积解析
- `ur_emit.h/cpp` + `ur_emit_main.cpp` — `.ur.h` 生成（`bin/csegen`）
- `cse_main.cpp` — 通用 CLI 驱动（`bin/cse`）

## 正确性与安全模型

通用模式下优化保持行为等价，八条机制：

1. **效果/纯度**：调用默认视为有副作用，`createCall` 仅对纯调用去重；不纯调用不被合并、
   不跨语句提取。**内建数学函数白名单只匹配限定名**（`std::sqrt`）的自由调用：
   裸名 `max`/`min`/`abs` 可能解析到用户定义的同名函数，把它当内建纯函数会让两次带副作用
   的调用被合并（`-s` 档同样中招）；同名**成员**调用（`s.sin(x)`）同样不查白名单——
   这里只拿得到方法的裸名。其余情况只有 `isPureFunction` 判真才算纯。
   任何"把两处出现折叠为一处"的重写，都要求被折叠的子表达式**深纯**
   （`isReferentiallyTransparent` 递归检查全部操作数）：`pure` 是逐节点的，
   `createBinaryOp` 一律置 `true`，所以 `f() + 1.0` 顶层 `pure` 为真而内部调用不纯，
   浅检查会把它当作可重复求值。
2. **内存屏障**：IRBuilder 预扫描得到只读根（未被写且未传入调用的变量）；
   仅只读根的 load 可去重/跨语句共享，可变/未知根的 load 每次独立，避免跨 store 复用。
   注意 `const T*` / `const T&` 参数**不再**自动视为只读根——`const` 只承诺"不通过它写"，
   不承诺没有别的指针/引用指向同一块内存（`f(x, x)` 会让 `f(const V& v, V& w)` 的 `v`、`w`
   指向同一对象）；`*`、`&`、`[` 形参一视同仁，都靠 `noAlias` 才恢复共享。
3. **赋值可见性**：变量按名字驻留，重新赋值不产生新节点，所以跨语句的改写必须显式检查
   写操作——`CSEPass` 只把子表达式提到"定义点到每个使用点之间都没有写入其操作数"的位置，
   `ValueProp` 只内联初值依赖的变量在整个函数内都没被写过的定义。这与第 2 条是同一条
   原则，只是作用于**标量变量**而非内存访问。
   **元素/成员写也要算写**：`a[i] = x;` / `p->f = x;` 写的是 lvalue 的**根**变量，
   各处扫描通过 `ir/location.h` 的 `collectWriteLocations`（共用一份实现）把它记进写集合——
   只记根是刻意的：可能有别的指针指向同一对象，此时"不知道是否有别名"只能取保守答案。
   `VarDecl` 不记入（它引入的是新名字，不是覆盖已有值）。
   这类写在 IR 里是**结构化语句** `AssignIR`（`targetExpr` + `value`），不随
   `lowerVectors` 变化——早先它只在向量降级路径下才结构化，于是默认档下是一段不透明
   表达式，写集合漏掉了整类写。
4. **支配安全**：出现在 `if`/`else`/循环内的子表达式标记为 nested，不参与顶层提取，
   避免把条件执行的计算提升为无条件计算。
5. **可投机性**：`analysis/purity.h` 的 `isSpeculatable` 回答"能不能在源码不会求值的
   位置求值"。它与 `isReferentiallyTransparent`（能不能重复/移动）是**两个问题**，只在
   陷阱处分歧：`1/k` 可以重复求值（值相同）但不可投机（k=0 时 trap）。
   CSE 的候选过滤对两类节点分别判定：
   - 语句级 nested（`if`/`loop` 里）→ 一律不提取；
   - 表达式级守卫（`c ? x : y` 的 x/y、`a && b` 的 b）→ **仅当**该节点
     `isSpeculatable` 才提取。于是 `k ? a*b : 0` 里的 `a*b` 仍被共享，而
     `k ? 1/k : 0` 里的除法不会。把整个三元/`&&` 提取是安全的（它保留内部短路），
     因此 `isSpeculatable` 对这两个操作符只检查条件/左操作数。
6. **作用域**：IRBuilder 维护作用域栈并对遮蔽变量 alpha-rename（如 `y__s1`），
   保证同名变量不跨作用域误合并。遮蔽判据覆盖三种情况：仍在作用域内的同名、
   已 pop 的兄弟作用域的同名、以及**region 只读不声明的外部名字**（全局/外层局部）——
   `double s = t; double t = a;` 里的 `t` 若与全局 `t` 共用节点，`s` 会被内联成 `a`。
   **生成名必须唯一**：所有 invent 出来的名字（builder `*__s*`、展开 `*__u*`、
   CSE `_cse_*`）都经 `IRModule::freshName` 分配，它保证结果不等于任何已驻留变量名
   或已预留名。
7. **循环展开前提**：仅当下列条件全部满足才展开，否则保留循环：
   - 循环体内每个局部声明都能被内联消除（初始化表达式为纯、非变量、不被重新赋值）
     或安全改名；这避免把同一局部变量跨迭代共享、进而被误当作循环不变量
     （例如 `uc` 的初始化含不纯调用时）。
   - 循环变量未被循环体改写（`i++`），否则常量替换会得到 `0++`。
   - 循环变量**不被循环语句之外读**，否则展开删掉它的声明会让那个读悬空
     （输入本身多半也不合法，但把一处错误放大成另一处仍然是错的）。
   - 循环体不含属于本循环的 `break`/`continue`，否则展开会把它们移出所命名的循环。
   「被改写」的判据是 `collectWriteLocations`，覆盖 `++`/`--` 与以表达式形式书写的赋值；
   「循环外被读」的判据是整函数读集减去本循环语句（body + cond + update）的读集。
8. **纯函数判定**：内建数学函数白名单只匹配**限定名**（`std::sqrt`）的自由调用。
   裸名 `max`/`min`/`abs` 可能解析到用户定义的同名函数，其副作用会被 CSE 合并掉；
   成员调用（`obj.sin`）同样不查白名单。未注册的调用一律视为不纯。

另外，CSE 工具**总是原样打印 IR 的树**：同优先级的右子节点一律保留括号，因为浮点下
`a - (b - c)`、`a / (b * c)` 甚至 `a * (b * c)` 与去掉括号后的形式都不等价。
发射还要避免相邻 token 粘连：前缀 `-`/`+` 的操作数若自身以同号开头（如折叠出的常量
`-5`）必须加括号，否则 `-(-5)` 会打成 `--5`；`for` 的初始化子句无论是声明还是赋值都要
发射，否则 `for (i = 0; …)` 会退化成 `for (; …)`，循环变量未初始化。

代数规则默认关闭，按**授权类型**分成四档（`CSEConfig`）：

| 开关 | 授权 |
|------|------|
| `assumeNumericCommutative` / `assumeNumericAssociative`（联合生效） | 交换/结合律重排（叶子排序、乘法链重排）。**精确恒等式不受此门控**（见 §5 代数简化），唯一的例外是符号规范化在 `0*inf` 处改变 NaN 的符号位，这一条只记录不门控 |
| `allowFpReassoc` | 浮点加法重排（`Reassociate`）、乘法链重排（`normalizeProduct`） |
| `allowUnsafeFpIdentities` | `x*0→0`、`x-x→0`、`0/x→0`、`x/x→1`、`x+0→x`、`0-x→-x`（在 `0`/`±inf`/`NaN`/符号零处改变结果）；以及 CleanupPass 的加法链合并（`x=0; x=x+a; x=x+b → x=a+b`，丢弃累加器前导 `0`） |

这三条零恒等式（`x*0`、`x-x`、`0/x`）替换的是**整个表达式**，所以替换出的常量必须
携带表达式的 int/float kind——对 double 发射整型 `0` 会把外层 `/ 4` 变成整除。kind
由 `joinNumericKind` 推断，而 load 的 kind 不可证（`Unknown`），于是授权由
`assumeFloatOperands` 单独给出：开启后 `0 * cell[i]` 折成 `0.0` 而不是原样保留。
**可证明的整型始终优先**：`(0 * k + 2) / 4` 在开启时仍是整数除法。一个有**浮点字面量**
的零乘积（`0.0 * a[i]`）不需要任何授权，按 usual arithmetic conversions 即可证明。

`-s`（保守档）关闭以上**全部假设/授权档**；默认档（FreeLB profile）全部打开。但
AlgebraicSimplify 的**精确档**（逐位精确的恒等式，见 §5）在任何档位都运行，包括 `-s`。
`-s` 与 `-r` 互不影响。

## 优化管线

默认管线按以下顺序执行（方括号为可选/受配置门控的环节）：

```
LoopUnroll → [Resolve] → ConstantFold → AlgebraicSimplify → [PostAlgebra]
  → [Reassociate] → CSEPass → [ExprRecombine → AlgebraicSimplify] → ValueProp → DCE → Cleanup
```

| Pass | 功能 |
|------|------|
| **LoopUnroll** | `for (i=0; i<N; ++i)` 展开为 N 条语句，并内联循环体局部变量 |
| **Resolve** | 插件 Pass（`resolvePass`）：解析 `latset::c/w` 为常量/点积（FreeLB） |
| **ConstantFold** | 编译期计算常量表达式 |
| **AlgebraicSimplify** | 精确恒等（`a*1→a`、`a-0→a`、`a/1→a`、`--a→a`、`a+(-b)→a-b`、`a-(-b)→a+b`、`x*2→x+x`、`(-a)*(-a)→a*a`、符号规范化，始终运行）+ 假设档（交换律排序、常量乘法合并，受 `numeric_`/`allowFpReassoc` 门控）；FP 恒等另有 `allowUnsafeFpIdentities` |
| **PostAlgebra** | 插件 Pass 槽位（`postAlgebraPass`），FreeLB 注入 **CounterProp**：直线计数器解析（`tensor[i]→tensor[0]`）、降级向量局部索引（`unew[1]→unew_1`）、常量条件折叠 |
| **Reassociate** | 加法项按全局出现频率重排，使对称语句共享不变前缀（需 `assumeNumericAssociative` **且** `allowFpReassoc`） |
| **CSEPass** | 跨语句公共子表达式提取（含干扰写检查，见「正确性与安全模型」第 3 条） |
| **ExprRecombine** | 乘法因子提取（-r 启用；除法与不纯节点不参与，见「表达式重组」节） |
| **ValueProp** | 内联简单初值到使用处（初值依赖的变量须在整个函数内未被写过） |
| **DCE** | 删除未使用的变量声明和赋值 |
| **Cleanup** | 末段清扫：删除没被读也没被写的零初始化声明、自赋值；合并链式赋值 |

### 插件注入

```cpp
static PassManager createDefault(const CSEConfig& config,
                                 bool enableRecombine = false,
                                 std::unique_ptr<Pass> resolvePass = nullptr,
                                 std::unique_ptr<Pass> postAlgebraPass = nullptr);
```

两个可选的项目专用 Pass 槽位：`resolvePass` 在循环展开之后、通用代数/CSE 之前
运行；`postAlgebraPass` 在 `AlgebraicSimplify` 之后、`Reassociate`/`CSEPass` 之前
运行。FreeLB 分别通过 `plugins/freelb/lattice_resolve` 与 `counter_prop` 提供，
核心 `src/` 不依赖任何项目。

## 核心特性

### 1. DAG 自然 CSE

IR 构建阶段通过哈希表实现自然去重：

```cpp
DAGNode* IRModule::createBinaryOp(OpCode op, DAGNode* lhs, DAGNode* rhs) {
    auto candidate = createNode(NodeKind::BinaryOp);
    candidate->op = op;
    candidate->operands = {lhs, rhs};
    candidate->recomputeHash();   // 语义字段全部写完之后才计算
    return intern(candidate);     // 结构相同则复用；不 pure 则原样返回
}
```

`createNode` 与 `intern` 都是 private：节点只能经由按种类划分的工厂产生，而工厂负责
「先填满语义字段 → 再算哈希 → 再入桶」这个顺序，`intern` 统一拒绝把不 `pure` 的节点
登记进表。文档化的协议与不变量检查见 `src/ir/dag_node.h` 与
`IRModule::verify()`（每个 Pass 之后在 debug 构建下断言）。

身份字段表**只写一处**：`CSE_DAGNODE_IDENTITY_FIELDS`（`dag_node.h`）列出决定「两个节点是否同一
节点」的全部字段，`DAGNode::sameContentAs()` 与 `computeHash()` 都展开这个宏——**被比较的集合与被
哈希的集合按构造相同**，二者不可能各自漂移。每个字段自己的规则（操作数按节点身份比、常量按原始位
比且 `isFloat` 参与身份）放在对应的 `identityEqual_<f>` / `identityHash_<f>` 助手里。
`tests/verify/verify_node_identity.cpp` 从外部钉住这张表：凡改变节点语义的字段都必须让两节点
`sameContentAs` 为假且 hash 相异——从宏里删一个字段即报红。

**优点**：零额外开销，在构建时自动完成
**局限**：只识别结构完全相同的表达式，不识别代数等价（如 `(a*b)*c` ≠ `a*(b*c)`）

### 2. 跨语句 CSE (CSEPass)

多迭代提取跨赋值语句的公共子表达式：

```c
// 优化前
feq[1] = rho * 0.111111 * (1.0 + 3.0 * u0 + u0 * u0 * 0.5 * 9.0 - 3.0 * u2 * 0.5);
feq[2] = rho * 0.111111 * (1.0 + 3.0 * (-u0) + (-u0) * (-u0) * 0.5 * 9.0 - 3.0 * u2 * 0.5);

// 优化后
auto _cse_0 = 3.0 * u2;
auto _cse_1 = rho * 0.111111;
auto _cse_2 = -u0;
feq[1] = _cse_1 * (1.0 + 3.0 * u0 + u0 * u0 * 0.5 * 9.0 - _cse_0 * 0.5);
feq[2] = _cse_1 * (1.0 + 3.0 * _cse_2 + _cse_2 * _cse_2 * 0.5 * 9.0 - _cse_0 * 0.5);
```

### 3. 结构化控制流

IR 使用嵌套结构表示控制流，而非 CFG：

```
ForLoopIR
├── init: StmtIR
├── cond: DAGNode*
├── update: DAGNode*
└── body: StmtIR (通常是 BlockIR)
```

### 4. 常量折叠 (ConstantFoldPass)

编译期计算常量表达式：

- `3 + 4 → 7`
- `2.0 * 3.0 → 6.0`
- `10 / 3 → 3.33333`

通过 `foldConst()` 递归遍历 DAG，当 BinaryOp 的两个操作数都是 Constant 时计算结果。

**例外：除数为 0 时不折叠。** 整数除零是未定义行为、浮点除零是 inf/NaN，`0` 是唯一对两者
都错的答案，而 IR 不携带类型、无法区分二者。该表达式原样保留，交给编译器诊断或求值。

**常量文本保留字面量拼写，int/float 类型参与身份。** `createConst` 只在调用方未给出拼写时
才自行格式化：整数值的浮点（`isFloat`）补 `.0`（`2.0`），整数值的整数发射为裸数字（`2`）；
源码里的 `1e16` / `0.5` / `-0.0` 按原样发射。常量的 **int/float 类型（`isFloat`）是节点身份的
一部分**——`4/3` 与 `4.0/3` 是不同节点、不同值，折叠与去重都必须保留这一位（早期只按数值
哈希，把二者并成一个，`(2.0+2.0)/(1.0+2.0)` 被折成整数除法 `4/3`）；`numText`（拼写）不参与
身份。codegen 对下标位置仍强制发射整数字面量，避免写出不编译的 `feq[1.0]`。

### 5. 代数简化 (AlgebraicSimplifyPass)

恒等消除 + 强度削减 + 叶子交换。规则分两档：**精确档**始终运行（每条都逐位精确，
含 `±0.0`/`±inf`/`NaN`/次正规数，故 `-s` 与裸 `CSEConfig` 也保留），**假设档**按
开关门控。`-s` 只关假设档，不再把精确档一并关掉。

**精确档（无需任何假设）：**
- `a * 1 → a`，`1 * a → a`
- `a - 0 → a`（两个符号零都精确），`a / 1 → a`
- `--a → a`（双重否定消除）
- `a + (-b) → a - b`，`a - (-b) → a + b`
- `x * 2 → x + x`（操作数须**深纯** `isReferentiallyTransparent`，否则会重复求值）
- `(-a) * (-a) → a * a`
- `a * (-b) → -(a * b)` 等符号规范化（**唯一例外**：`0 * inf` / `inf * 0` 产生的
  NaN **符号位**与源码不同——源码得 IEEE "invalid" 形式，改写后传播被取反操作数的符号。
  NaN 的符号/payload 不是 IEEE-754 的可观察值，`isnan` 与所有比较都不区分，故仍留在
  精确档；放到 `numeric_` 后面会让 `-s` 丢掉 equilibrium 正反方向共享的收益（93→84）。
  `verify_exact_identities.cpp` 同时钉住两半：非 NaN 操作数逐位精确，NaN 仍是 NaN）

**假设档（需 `numeric_` = 交换律 ∧ 结合律）：**
- 交换律排序：叶子节点按常量优先、变量名字典序排列，促进后续 CSE 匹配
- 乘法链重排：常量因子合并 + 左结合重建（另需 `allowFpReassoc`）

**另需 `allowUnsafeFpIdentities`（在 `0`/`±inf`/`NaN`/符号零处改变结果）：**
- `a + 0 → a`，`0 + a → a`（`a = -0.0` 时和为 `+0.0`）
- `0 - a → -a`（`a = +0.0` 时差为 `+0.0`，而 `-a` 是 `-0.0`）
- `a * 0 → 0`，`0 * a → 0`
- `0 / a → 0`
- `a - a → 0`，`a / a → 1`

注：`x + 0` 因符号零**不精确**（故留在 `allowUnsafeFpIdentities`），而 `x - 0` 精确
（在精确档）——两者不可混为一谈。常量匹配要求 `symbol.empty()`：符号常量（如
`ns::w<T>(1)`，为发射保留）带真实数值但**不是**数值字面量，不能被折掉（与
`normalizeProduct` 的常量折叠守卫一致）。

### 6. 表达式重组 (ExprRecombinePass)

分配律方向的公因子提取（`-r` 启用），**只对乘法生效**：

- `a * x + a * y → a * (x + y)`
- `a * x + b * x → (a + b) * x`
- `a * x - b * x → (a - b) * x`

四种左右组合都会尝试（同一乘积的任一因子都可作公因子）。两条硬约束：

- **除法永不参与**。IR 不携带类型信息，`a / x + b / x` 只在精确除法下等于 `(a + b) / x`；
  对整数 `3/2 + 1/2 == 1` 而 `(3+1)/2 == 2`。因此既不能输出 `/` 形式，更不能输出 `*`
  （后者是历史缺陷，已修）。
- **公因子必须通过 `samePureExpr`**（`a->pure && b->pure && sameExpr(a, b)`）。该重写把
  两处文本出现折叠为一处，所以带副作用的调用、可写位置的 load 都不能作公因子——IR 刻意
  让这类节点保持独立，结构相同并不意味着可以互换（否则 `a * f() + b * f()` 会丢一次调用）。

- `a * x + a → a * (1 + x)`、`a * x - a → a * (x - 1)` 也在支持之列。它们此前是**死代码**
  （函数开头的前置检查要求 `+`/`-` 两侧都是二元表达式，而这两种形式必有一侧是裸变量），
  现已可达；由于默认 `-r` 档还会跑 `Reassociate`，`tryFactorAddSub` 会把
  `a*x + a` / `a*x - a` 规范化回"乘积在左"的形态再匹配。

第三条约束与交换律有关：`la*lb + ra*la → la*(lb+ra)` 这类**跨组合**、以及上面那条形态
归一，都需要 `+`/`*` 满足交换律，因此挂在 `assumeNumericCommutative` 上。直组合
（`la==ra`、`lb==rb`）不需要交换律，无条件生效。

覆盖：`tests/fixtures/recombine.cpp`（`-r` 成本回归 28→24 flops，另有两项 FLOP 中性的
形状检查）+ `tests/verify/verify_recombine.cpp`（差分执行校验：乘法因子提取数值等价、
除法与整数除法语义不变、不纯调用求值两次）。

### 7. 值传播 (ValuePropPass)

内联简单初值到使用处，消除中间变量：

```cpp
double val = a;       // 初值是 Variable → 可内联
double t = val * val; // 内联后: double t = a * a;
```

`double val = a * x;` **不会**被内联——`isCheapShareableValue`（`analysis/purity.h`）只接受
Constant / Variable / MemberAccess / ArrowAccess（后者基须为 Variable），
任何 `BinaryOp` 都会被拒绝（内联=复制表达式，只允许零成本的形式）。

**安全约束：**
- 只内联上述"零成本"表达式；
- 被内联的变量本身不得被重新赋值（预扫描 `collectWriteLocations` 得到 `reassigned`，
  再经 `AliasInfo::closeWrites` 把引用局部变量的写展开到其 referent）；
- **不得内联某个引用的 referent**（`AliasInfo::isAliasedReferent`）：`double& r = v;` 的初值是
  *lvalue 绑定上下文*，把 `v` 换成它的值会得到 `double& r = 0.5;`（对非 const 引用不合法；
  对 `const&` 则静默改绑到临时）；
- **初值读取的变量不得在整个函数内被写过**（`initDepsStable` 经 `AliasInfo::writeAffectsRead`
  查同一 `reassigned`——读名先沿引用边闭包，故读 `s3`（引用 `b`）会被写 `b` 影响）。否则
  `double t = a; a = b; return t;` 会被改写成 `a = b; return a;`；
- 不内联函数参数**本身**——但"以只读参数为初值的局部"可以内联（参数未被写过就是稳定的）；
  这两个集合（`blocked` 与 `written`）必须分开维护；
- 只处理 `VarDecl` 初值；`Assign` 的右值不内联（`toRemove` 只能删声明，
  内联赋值会留下残余语句）。

### 8. 死代码消除 (DCEPass)

删除未使用的变量声明和赋值：

```cpp
double unused = x * y;  // 使用次数为 0，删除
double t = a + b;       // 使用次数 > 0，保留
return t;
```

迭代至不动点，因为删除可能暴露新的死代码。

**三条独立的不删除条件**（每一条都曾是生成错误代码的成因）：

| 条件 | 反例 |
|------|------|
| 复杂 lvalue（`a[i] = v;`、`p->f = v;`）写的是内存，lvalue 不是一个可推理的名字 | —— |
| 目标不是本函数**自己声明**的局部（形参 + 本模块的 `VarDecl`），可能是全局 | `g = a;` 曾被整条删除，函数外看不到赋值 |
| 右值本身有副作用（`hasSideEffect`：不纯调用 / `++` `--` / 表达式形式的赋值） | `unused = y++;` 曾被删除，`y` 的增量随之消失，函数返回 `a` 而非 `a+1` |

声明可删的条件同样是两个：**既没被读也没被写**。`countReadNames` 只数出现次数（读），
对被写入过、却从没被读的名字一无所知——保留了一条 `unused = y++;` 之后，
若把 `double unused` 删掉，留下的语句就引用了一个不存在的变量。
`reads` 与 `written` 每轮重算，所以"删除赋值 → 下一轮释放声明"会自行收敛。

DCE 之后还跑一个 **CleanupPass** 做末段清扫：删除没被读**也没被写**的零初始化声明
（与 DCE 同一条 written 判定——只看"没被读"会把 `unused = y++;` 的声明删掉、留下
编译不过的悬空赋值，合并时曾真实发生）、删除自赋值（`u_value[i] = u_value[i];`）、
合并链式的声明赋值与赋值语句。`pruneBlock` 递归进入 `if`/`for` 体与嵌套 `{ }`（见
`pruneNested`），把每一层作用域都当独立块清理；`allowUnsafeFpIdentities` 门控贯穿
每一层。分析助手（`countReadNames` / `collectWriteLocations`）本就是深递归，缺的只是
对嵌套语句的**删除**。

**链式合并受 `allowUnsafeFpIdentities` 门控**：`x = 0; x = x + a; x = x + b` 合并为
`x = a + b` 时**丢掉了累加器的前导 `0`**，这正是 `x + 0 → x` 那条符号零恒等——
`a = b = -0.0` 时原式得 `+0.0`、合并式得 `-0.0`（`1.0/x` 因此从 `+inf` 变 `-inf`）。
它挂在同一个授权开关上：默认档合并，`-s` 档保留 `x = 0; x = x + a; …`。
（`-s` 的 `cse_main.cpp` 分支此前漏清该开关，靠 `AlgebraicSimplify` 的 `numeric_` 门控
掩盖；Cleanup 无 `numeric_` 门控，故一并补齐。）

### 9. 前端 C++ 语法支持

| 语法 | 状态 |
|------|------|
| `::` 命名空间限定 | 已支持 |
| `using` 类型别名 | 已支持 |
| `namespace` 定义 | 已支持 |
| `typename` / `class` 模板参数 | 已支持 |
| `static` / `const` / `inline` 修饰符 | 已支持 |
| `unsigned int` 复合类型 | 已支持 |
| `__any__` / `__host__` 等双下划线宏 | 已跳过（可配置） |
| `#include` / `#ifdef` 等预处理指令 | **不建模**：词法器发出 `PreprocessorDirective` 词法单元（不再静默丢弃整行），区域提取器据此把 `CSERegion::hasDirectives` 置位，驱动把该区域**原样透传**并计入 `skipped-unsupported`。因为 IR 没有指令节点，按 AST 重建会删掉 `#if` 守卫并把 `#if 0` 的体复活。`#` 在注释/字符串里是普通文本（那两种字面量先整块成词），所以不会被误判 |
| `T{1}` 括号初始化 | 已支持（可配置简化） |
| `++k` / `--k` 前缀运算符 | 已支持 |
| `k++` / `k--` 后缀运算符 | 已支持（最长匹配成词，否则 `a++ + b` 与 `a + +b` 同流） |
| 一元 `+` | 已支持 |
| 科学计数法 / 浮点后缀字面量 | 已支持（`1e16` 曾lex成 `1` + 标识符 `e16`） |
| `f(void)` 空形参列表 | 已支持（`void` 是类型关键字，需在 `)` 前特判） |
| 字符串 / 字符字面量 | **不支持**（`"..."` 仍是 parse error；区域会被跳过并原样输出） |
| 模板函数调用 `latset::c<LatSet>(k)` | 已支持（作为不透明调用） |
| 模板实参与 `<` 比较的区分 | 已支持（`ident<…>` 仅在实参列表合法、且 `>` 后紧跟 `(`/`::`/`.`/`->`/`[` 时才算模板 id，否则按 `<` 比较解析） |

### 10. IR 工具函数 (stmt_walk.h / location.h / ir_utils.h)

提供 pass 通用的工具函数，不侵入 IRModule：

**表达式槽位遍历（`stmt_walk.h`）**
- `forEachExpr(stmt, visit, includeLvalue=true)` —
  **语句拥有的表达式槽位**，就地遍历。`Assign` 贡献两个槽位：lvalue 与 value；引用语义允许改写。
- `forEachExprDeep(stmt, visit, includeLvalue=true)` — 同上，含嵌套语句。

**读写/别名模型（`location.h`，见「正确性与安全模型」第 3 条）**
- `locationRoot(node)` / `locationOf(node)` — lvalue 的根变量名（`a[i].m` → `a`）/ 位置
- `collectReadNames` / `countReadNames` — 读集 / 每个名字的读次数（经 `forEachExprDeep`）
- `collectWriteLocations` — 一条语句写了哪些位置
- `LocationSet` / `mayAlias` / `AliasInfo` — 位置集合、别名关系、引用 referent 闭包
- `AliasInfo::writtenBy` / `isAliasedReferent` / `writeAffectsRead` — 闭包后的写集 / 某名是否为某引用的
  referent（不得值传播）/ 某写集是否影响对某名的读（读名先沿引用边闭包——与写集闭包的镜像）

**其余（`ir_utils.h` 与 `analysis/purity.h`）**
- `substitute(mod, root, name, replacement)` — 在 DAG 子树中替换变量
- `foldConst(mod, node)` — 常量折叠（BinaryOp 两个 Constant 操作数；除数为 0 时不折叠；
  结果保留操作数的 int/float 类型，整数除法不产生小数）
- `hasObservableEffect(node)`（`analysis/purity.h`）— 丢弃这个值会不会丢掉副作用（不纯调用 /
  `++` `--` / `=` 形式的赋值）。早先只看调用的 `hasImpureCall` 已删除，DCE 曾因此删掉
  `unused = y++;` 里的自增

**为什么要有 `stmt_walk.h`**：表达式槽位的清单原先在约十个 pass 里各写一遍
（`countReadNames`、DCE、五个重写型 pass、CSE 的三处收集/替换、cost model），
每次新增一个槽位都要同步十来处，`AssignIR::targetExpr` 就是这么被漏掉的。
现在清单只有一份；新增语句种类或槽位时只改 `forEachExpr`。
`includeLvalue=false` 只给一个调用者用：CSEPass 不能把 lvalue 换成临时变量
（那会把"写某个位置"变成"写临时变量"），所以它显式排除该槽位、只替换索引里的子表达式。

### 11. 构建系统

分离静态库 + 动态库 + 可执行文件：

```
bin/
├── libcse.a      # 静态库
├── libcse.so     # 动态库
├── cse           # 通用 CSE CLI（静态链接 libcse.a）
└── csegen        # FreeLB .ur.h 生成器（静态链接 libcse.a）
```

两个入口分别在 `plugins/freelb/cse_main.cpp` 与 `plugins/freelb/ur_emit_main.cpp`；
库目标由 `src/` 与 `plugins/freelb/` 下除这两个入口外的全部 `.cpp` 组成。

`csegen` 用法：`csegen <input.h> <output.h>`，`input` 的 basename 决定 include 与
namespace，须为 `moment` / `equilibrium` / `force` 之一（FreeLB 的 `.ur.h` 生成）。

### 12. 循环展开 (LoopUnrollPass)

将 `for (i = 0; i < N; ++i)`（N 为字面常量）展开为 N 条语句：

- 克隆循环体，用常量替换循环变量（复用 `substitute`）
- 展开后的语句直接拼接进父 Block，保证跨语句 CSE 可见
- 循环体内声明的局部变量若仅在该循环体内使用，则内联并删除声明

展开前还要确认展开本身是安全的，否则整个循环保持原样（不展开）：

- **循环变量不得被循环体改写**：`for (i=0; i<4; ++i) { s += a; i++; }` 里的 `i++`
  会随常量替换变成 `0++`。判据由 `collectWriteLocations` 提供，覆盖 `++`/`--`、以
  表达式形式书写的赋值，以及结构化 store 的根。
- **循环体局部若被改写**（`t++;`）则改名（`t__u0` …）而非内联，否则内联会把 `t`
  代进它自己的 `++`（`(a + 1.0)++`）。
- **循环体内出现属于本循环的 `break`/`continue`** 时拒绝展开：展开会把语句拼进父
  Block，从而把它们移出所命名的循环。嵌套循环自身的 `break`/`continue` 不算
  （遍历不进入嵌套 `ForLoop` 的循环体）。前端没有这两个关键字，`break;` 以「名为
  `break` 的裸变量表达式语句」形式到达 IR。
- `for` 的初始化子句若为赋值（`for (i = 0; …)` 而非声明）必须原样发射；codegen 只
  认声明形式时会把它丢掉，得到 `for (; …)`，循环变量因此未初始化。

### 13. 加法重结合 (ReassociatePass)

将 `+`/`-` 链展平为带符号项，按每个带符号项在函数中的出现频率降序重建。
出现频率高的项排在前面，使对称语句（如 D3Q19 的 ± 方向对）形成相同的
不变前缀（例如 `var0 + 4.5*uc²`），随后由 CSE 提取，每条语句只剩 `± 3*uc`。

### 14. FreeLB 常量解析 (plugins/freelb/lattice_resolve)

- 硬编码 D2Q5/D2Q9/D3Q7/D3Q15/D3Q19/D3Q27 方向向量与权重查找表
  （与 FreeLB `lattice_set.h` 一致，`tests/verify/check_lattice.py` 防漂移）
- `latset::w<LatSet>(k)` → 数值用于权重分组，**代码输出保留声明的访问器形式**
  （如 `latset::w<D3Q19<double>>(1)`），避免烘焙十进制字面量带来的精度/类型转换
  问题；同值权重收敛到代表索引，权重分组不受影响
- `latset::c<LatSet>(k)[i]` → 常量分量（整数，精确）
- `u * latset::c<LatSet>(k)` → 标量点积 `u[0]*cx + u[1]*cy + u[2]*cz`
- 对方向向量做符号规范化（提取前导 -1），使相反方向成为精确取反，
  配合 `(-a)*(-a)→a*a` 共享 `uc²`
- 要求 k 为编译期常量（由 LoopUnroll 保证）

## 已知局限

| 局限 | 说明 | 影响 |
|------|------|------|
| **结合律仅限加法** | 乘法链只在常量因子层面重排 | 一般结合律仍不识别 |
| **类型系统不完整** | 类型用字符串表示 | 不支持类型检查、模板实例化、类型推导 |
| **区域内无错误恢复** | 区域解析失败时整块跳过、原样输出，退出码区分全部成功 / 部分跳过 / 全失败 | 一个区域内只报第一个错误；跨区域已能继续 |
| **仅处理标记区域** | 只解析 `//@cse` 标记的代码 | 无法跨区域优化 |
| **作用域只覆盖本区域** | alpha-rename 处理本函数内的遮蔽，以及本区域**读过**的外部名字；但工具看不到 region 之外的声明 | 若 region 外有一个与 region 内局部同名、且 region **从未读**的名字，两者仍可能共用节点。实际影响面很窄（该名字在 region 内无引用），彻底修法需要扫描 region 外的顶层声明 |
| **数组复合赋值未建模** | `a[i] += x` 未展开为 `a[i] = a[i] + x` | 仅与变量 `x += y` 等价；`AssignIR::compoundOp` 保留原样求值 |
| **无通用 constexpr** | 仅模式匹配已知 lattice | 其他 constexpr 调用仍不透明 |
| ~~引用型形参未纳入别名判据~~ | **已修**：`&` 与 `*`/`[` 一起进 `_pointerParams`（形参与引用型局部） | `f(const V& v, V& w)` 以 `f(x, x)` 调用时不再跨写复用 `v` 的 load（`ref_alias` 夹具钉住）；`noAlias` 仍是恢复共享的唯一出口 |
| **写集合只看 lvalue 的根** | `collectWriteLocations` 对元素/成员写只记根变量名 | 若两个不同根实际别名同一对象，检查会认为"没写"；`noAlias` 与"根名不同即不别名"是当前的全部依据 |
| **写集合不含声明** | `VarDecl` 不计入写集合 | 名字遮蔽已做 alpha-rename，故不会与旧绑定混淆；新增读写分析时注意这一点 |
| **break/continue 阻断展开** | 循环体内出现属于本循环的 `break`/`continue` 时不展开该循环 | 该循环体不参与跨语句 CSE（正确性优先）；`break;`/`continue;` 以「名为 `break`/`continue` 的裸变量表达式语句」形式到达 IR，因为前端没有这两个关键字 |
| **FLOP 口径是「按引用次数」** | `cost_model` 按**每个使用点**（引用次数）计费，而非 DAG 唯一节点数；物化临时只算其定义一次（`totalNodes` 仍为唯一节点数） | 它近似的是**生成代码**的开销：DAG 已哈希去重，而 codegen 在每个使用点重印未物化的节点。`for (i<4) a+=b;` 展开出的四条 `a = a + b;` 记 4；同一条语句内重复的子表达式也逐处计（旧口径只计一次，是已修掉的近似）。规则由 `verify_cost.cpp` 钉住，实现是反向拓扑多重度传播（O(V+E)） |
| **常量文本不进身份** | `numText` / `symbol` 不是节点身份的一部分 | 同一个常量节点只有一份拼写，可能被算术位置与下标位置共用；下标由 codegen 保证整型 |
| **`if`/`else` 体内不做跨语句 CSE** | `nested` 是一个布尔标记：节点只要在条件执行里出现过就整体排除 | 过度保守而非不安全：`if (n>0) { t=a*b; u=a*b; }` 完全不提取。改成支配树即可放开（见 `docs/cse-round6-audit.md` §4 B1） |
| **CSE 提取是 O(n²)** | 每提取一个候选就重扫全部语句、重算写集 | 400 个独立候选约 7 秒；大内核会到分钟级（§4 C1） |
| **解析深度上限 512** | 表达式嵌套超过 512 层报 `CSE-PARSE-DEPTH` 并透传区域 | 防的是栈溢出崩溃；真实内核远低于此 |
| **明确搁置的改造** | B1 支配树（放开 if 体内 CSE）、C1 CSE 单遍化（O(n²)→O(n)）、C2 变量版本化（消除按名驻留 + 写集补偿层）、E1 region 外声明扫描、E2 AssignIR 双表示统一、P2-7 `IRBuilder::buildExpr`/`buildValue` 双路径合并 | 2026-10 审计（`docs/cse-round6-audit.md` §7）评估后**有意搁置**：均为能力提升或整洁收益，无正确性增量，而回归面集中在核心 pass。重启条件：C2 需「忘问写集」类缺陷再现成模式；B1 需有真实内核因 if 体内 CSE 缺失而收益可见；E1 需 A3 的最小修法（读/写外部名登记）再暴露新缺口；P2-7 需两条镜像路径出现行为漂移的缺陷（它们是「同一判断多份实现」的已知形状，当前靠同一套孪生测试兜住——合并须带向量开启档逐位不变的验证） |

> 注：不纯调用合并、跨 store 复用 load、分支外提、遮蔽、`==`/`<=` 运算符等
> 正确性问题已在通用安全模式下修复（见「正确性与安全模型」）。

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
| **CounterProp 失效细化** | `updateKnown`/`entrySeedFor` 遇到任何 wildcard 写集就清空整个计数器表；可细化为只失效「实际被写」的计数器 | 待实现（性能优化，非性能正确性） |

### 低优先级

| 方向 | 描述 | 状态 |
|------|------|------|
| **错误恢复** | 遇到 `;` 或 `}` 时同步，继续解析 | 待实现 |
| **CFG 支持** | 支持 break/continue/goto | 待实现 |
| **parser 重复收敛** | `parseType`/`parseFullType` 的限定名/模板实参平衡扫描逐行近似重复（~65 行），`template<...>` 跳过前瞻三份手写复制；可收敛为带参数的单函数 + `skipTemplateParams()`（约 −100 行） | 机会性做：下次因新特性改动 parser.cpp 时顺手合并，不单独立项（纯整洁收益，无判据可红） |

### 延后项（可选）

| 项 | 描述 | 状态 |
|----|------|------|
| **Cleanup 递归清理** | `CleanupPass::pruneBlock` 递归进入 `if`/`for` 体与嵌套 `{ }`（`pruneNested`），每层作用域独立清理，`allowUnsafeFpIdentities` 门控贯穿。夹具 `cleanup_nested` + `run_tests.sh` 形状检查钉住 | **已实现** |
| **生成物自赋值回归断言** | `tests/verify/check_self_assign.py`（正则 backreference，注释盲）在 csegen 冒烟中对三个 `.ur.h` 断言不存在 `a[i] = a[i];` | **已实现** |

## 测试覆盖

| 测试 | 覆盖功能 |
|------|----------|
| `tests/fixtures/basic_cse.cpp` | 基本 CSE / 乘积链因式分解（10 flops after） |
| `tests/fixtures/features.cpp` | 多函数综合：结构体/方法、模板、箭头/成员访问、循环、分支（51 flops after；含 runtime-bound 循环，报告为下界） |
| `tests/fixtures/namespace_case.cpp` | `//@cse` 区域内 namespace 的函数/结构体被优化并输出（4 flops after） |
| `tests/fixtures/equilibrium_d3q19.cpp` | FreeLB D3Q19 loop 版：展开 + 常量解析 + 重结合（向量加权后 84 flops） |
| `tests/fixtures/safety_cases.cpp` | 正确性风险用例：不纯调用/load-store/分支/比较运算符/遮蔽（21 flops after） |
| `tests/fixtures/cost_nested.cpp` | 成本模型：三角 dim 循环、runtime-bound 循环（计一次并标记 unknownLoops）、向量加权、helper call（355 flops after） |
| `tests/fixtures/cost_descending.cpp` | 成本模型：递减循环（`>=`+`--`、`>`+`-=k`）正确计数，runtime-bound 循环标记 unknownLoops（35 flops） |
| `tests/fixtures/recombine.cpp` | 表达式重组（`-r`）：乘法因子提取生效，除法/整数除法/不纯调用不被改写（24 flops after） |
| `tests/verify/verify_equilibrium.cpp` | D3Q19 生成代码与参考实现数值一致性 |
| `tests/verify/verify_safety.cpp` | 安全用例的差分执行校验 |
| `tests/verify/verify_recombine.cpp` | `-r` 输出的差分执行校验（数值等价 + 副作用出现次数） |
| `tests/fixtures/parens.cpp` | 括号保持：右子节点在等高优先级时必须保留括号（默认档 + `-s` 档，19 flops） |
| `tests/fixtures/store_aware.cpp` | 跨语句改写必须尊重写操作：CSE / ValueProp / `const` 别名；成员、箭头、元素写；只写不读的局部必须活过 DCE；共享索引要两侧同时改写（21 flops，`-s` 档） |
| `tests/fixtures/float_identities.cpp` | `-s` 档必须保留 0 / ±inf / NaN 处的 IEEE 语义（8 flops） |
| `tests/verify/verify_parens.cpp` | 括号保持的差分执行校验（覆盖 `-` `/` `*` `%` 的各种右子节点） |
| `tests/verify/verify_store_aware.cpp` | 跨 store 重写的差分执行校验（含**真别名**调用） |
| `tests/fixtures/ref_alias.cpp` | 引用形参与指针同一条别名规则：`f(const V& v, V& w)` 以同一对象调用时不得跨写复用 `v` 的 load（1 flops，`-s` 档） |
| `tests/verify/verify_ref_alias.cpp` | 真别名调用 `f(x, x)` 的数值校验（修复前得 2.0，应为 10.0） |
| `tests/verify/verify_float_identities.cpp` | 特殊值下的数值校验（NaN 是否仍然产生） |
| `tests/verify/verify_config.cpp` | 库层 `CSEConfig` 契约（CLI 无法隔离的新开关） |
| `tests/fixtures/speculation.cpp` | 惰性求值不得被投机：`k ? 1/k : 0` 与 `k && (10/k)` 写两遍时，除法不得被提到条件之外（`k=0` 曾 SIGFPE）；对照 `k ? a*b : 0` 里的乘积仍应被提取（8 flops，`-s` 档 + 孪生差分 4 对） |
| `tests/fixtures/fresh_names.cpp` | 生成名唯一性：用户变量 `_cse_0_4`、形参 `t__u0`、局部 `x__s1` 分别撞上 CSE/展开/builder 的命名空间，必须不共享节点（3 对孪生） |
| `tests/fixtures/shadow_global.cpp` | region 只读不声明的外部名字：`double s = t; double t = a;` 里的局部 `t` 必须 alpha-rename，否则 `s` 被内联（基线 2995 处不匹配，3 对孪生） |
| `tests/fixtures/purity_names.cpp` | 内建白名单只认限定名：用户自定义 `max` 的两次调用必须各生效一次（调用计数器为判据）；对照 `std::sqrt` 仍去重（形状检查） |
| `tests/fixtures/loop_escape.cpp` | 循环变量逃逸：`return s + i;` 后的循环不得展开（形状检查，输入本身不合法 C++）；对照：受控计数器仍展开、形参同名仍展开、兄弟循环各自展开（15 flops，3 对孪生） |
| `tests/verify/verify_purity_names.cpp` | 上述 `max` 的调用计数 + `std::sqrt` 的值校验 |
| `tests/fixtures/comment_braces.cpp` | 区域提取：注释里的 `{` `}` 不参与括号计数（默认档 + `-s` 档，4 flops） |
| `tests/fixtures/dead_store_effects.cpp` | DCE 保留带副作用的死存储；纯死存储仍删除（3 flops + 三条形状检查） |
| `tests/fixtures/constant_edges.cpp` | 除零不折叠、`-0.0` 与 `1e16` 拼写保留、共享字面量的下标仍为整数（6 / `-s` 6 flops + 三条文本检查） |
| `tests/fixtures/void_param.cpp` | `f(void)` 可解析，普通形参列表不受影响（4 flops） |
| `tests/verify/verify_comment_braces.cpp` | 注释含括号的区域的数值校验 |
| `tests/verify/verify_dead_store_effects.cpp` | 自增/自减、全局写、不纯调用、纯死存储的数值校验 |
| `tests/verify/verify_constant_edges.cpp` | 除零得 inf、`-0.0` 的符号、共享字面量下标的数值校验 |
| `tests/verify/verify_void_param.cpp` | `(void)` 三种形态的数值校验 |
| `tests/fixtures/loop_unroll_semantics.cpp` | 循环展开：复合存（`out[i] += x` / `p[i].v += x`）保留 `compoundOp`；含不可共享 load 的循环体局部不得内联越过 store（12 flops，默认档 + `-s` 档） |
| `tests/verify/verify_loop_unroll_semantics.cpp` | 展开循环内复合存与「先读后写」的差分执行校验（返回值与被改写缓冲区同时比较） |
| `tests/fixtures/value_prop_chain.cpp` | 值传播：平凡局部变量的链式定义（`u = t` 且 `t = a`）不得在被引用时删掉声明；含局部遮蔽同名全局的静默错绑定变体（6 flops，默认档 + `-s` 档） |
| `tests/verify/verify_value_prop_chain.cpp` | 链式局部变量（二/三链、成员 load 起链、多次使用、遮蔽全局）的差分执行校验 |
| `tests/fixtures/cleanup_store_index.cpp` | Cleanup：只在 store 左值（`a[x] = v` / `p[i].v = v`）中读取的局部变量不得删声明；含遮蔽同名全局的静默错槽变体（5 flops，默认档 + `-s` 档） |
| `tests/verify/verify_cleanup_store_index.cpp` | store 索引局部变量（纯左值 / 左值内表达式 / 成员元素 / 遮蔽全局）的差分执行校验（返回值与被改写缓冲区同时比较） |
| `tests/fixtures/semantics_fixes.cpp` | 语义安全修复回归（两批）：逻辑运算符、codegen 括号、复合存、跨调用写、不纯调用被 `*0.0` 丢弃、整数除法、遮蔽、链式合并、分数起点循环；第二批的 `for` 初始化赋值、循环变量/局部被 `++` 改写、`break`/`continue` 阻断展开、负常量前导 `-`、`<` 后接 `>` 的区域解析（默认档 77 / `-r` 档 77 / `-s` 档 76 flops + 一条 `<`/`>` 形状检查） |
| `tests/verify/verify_semantics_fixes.cpp` | 上述用例的差分执行校验（`sf_*` 对未标记的 `ref_*` 孪生），默认档 + `-s` 档各跑一次 |
| `tests/fixtures/ref_param_store.cpp` | DCE 不得删除对引用的存储（引用形参或引用型局部）；指针形参自身的存储仍应删除（9 flops） |
| `tests/verify/verify_ref_param_store.cpp` | 引用存储存活、指针自存仍死的差分执行校验 |
| `tests/fixtures/loop_unroll_inline.cpp` | 展开：循环体局部若其初值读取了循环体写入的变量，必须改名而非内联（默认档 39 / `-s` 档 40 flops） |
| `tests/verify/verify_loop_unroll_inline.cpp` | 展开内联越过写入的差分执行校验 |
| `tests/fixtures/struct_region.cpp` | `//@cse` 标记的纯数据 struct 必须输出（2 flops） |
| `tests/verify/verify_struct_region.cpp` | 纯数据 struct 输出的编译校验 |
| `tests/fixtures/region_toplevel.cpp` | `//@cse` 标记的顶层声明不得被并进下一个函数的区域（3 flops） |
| `tests/verify/verify_region_toplevel.cpp` | 顶层声明输出的编译校验 |
| `tests/verify/verify_builder_guards.cpp` | 库层：空语句槽位必须被拒绝为 `CSEError` 而不是解引用空指针 |
| `tests/verify/check_lattice.py` | 引擎 latset 表 vs FreeLB `lattice_set.h` 防漂移 |
| `tests/csegen/{equilibrium,force,moment}.h` | `csegen` `.ur.h` 生成冒烟（Cell/TLatSet/TLatSetD/CellType 各形态） |

> 所有工具产物（`*.cse`、`*.ur.h`、验证器可执行文件）写入临时目录，源码树不被修改。
> 数值正确性以夹具 + 验证器成对覆盖（equilibrium、safety、recombine、parens、store_aware、ref_alias、
> float_identities、mixed_ops、write_visibility、effect_duplication、frontend_forms、
> comment_braces、dead_store_effects、constant_edges、void_param、loop_unroll_semantics、value_prop_chain、cleanup_store_index、
> semantics_fixes、ref_param_store、loop_unroll_inline、struct_region、region_toplevel 等——
> 上表为代表性条目，全部夹具与判据的权威清单在 `tests/README.md` 与 `run_tests.sh`），
> 而非 golden-diff。
> 库层契约有两类：CLI 够不到的开关与守卫（`verify_config`、`verify_builder_guards`、
> `verify_counter_prop`——counter-prop 是插件注入的 pass，CLI 不注册它），以及
> **pass 契约**（`verify_pass_contracts.cpp`：每个默认管线的 pass 必须能触发、
> 只有 LoopUnroll 可不保效果、跨语句 pass 必须声明写集契约 `Pass::crossesStatements`）。
> 注意 **FLOP 回归对某些缺陷无效**：括号丢失不改变 flops，`a*x ± a` 的提取是 FLOP 中性的，
> 跨成员写的错误共享也恰好省下同样的 flops（store_aware 修复前后是同一个数），
> `y++` 与 `-0.0` 更是完全不进 flops——这几类只能靠数值验证器或生成文本的形状检查，
> 因此 `run_tests.sh` 里另有一组 grep 形状检查（重组形态、强度削减、死存储、常量发射、
> store 索引、`<`/`>` 区域解析、**穷举 switch**——`check_stmt_switches.py` 断言
> 每个 `switch (stmt->kind)` 显式穷举或带 `// partial:` 标记，见「编码约定」）。
> FreeLB 侧 `verify_*.py` 是**数值**校验：解析两个文件、按公式求值再逐个比较，
> 不做文本比对（所以重命名 `_cse_*` 这类内部标识符它发现不了，字面量拼写变化也不影响它）。
> 入口是 `make test`（`tests/run_tests.sh`）：FLOP 代价回归（默认档 + `-r` 档 + `-s` 档）→
> 形状检查 → 数值校验 → 库层契约（`verify_config` + `verify_builder_guards` + pass 契约）→ `csegen`
> 冒烟；`check_lattice.py` 与 FreeLB 侧 `verify_*.py` 仅在存在 FreeLB checkout
> （`FREELB=` 或 `~/FreeLB`）时运行，否则跳过。

### 成本模型（重设计）

成本模型现在按 **标量 FLOP、向量按 lane 加权** 计算：向量算子的代价为其 lane
数，向量点积为 `2d-1`，纯 helper/intrinsic 调用由 FreeLB 插件按名给定代价
（`getnorm2 -> 2d-1` 等），未知调用与无法解析的循环会被显式计数
（`unknownLoops` / `unmodeledCalls`）。子表达式按 **引用次数** 计费：每个使用点
一次，物化临时只算其定义一次（`totalNodes` 仍是唯一节点数），循环体按执行次数
缩放（三角 `for (b=a; b<d; ++b)` 嵌套精确计数）。详见 `docs/cost_model_redesign.md`。

### D3Q19 equilibrium 实测（cse -c，向量加权 + 按引用次数）

`tests/fixtures/equilibrium_d3q19.cpp`（通用 `cse -c` 入口，引擎回归用）：

| 版本 | FLOPs |
|------|:-----:|
| 原始循环版（Before） | 323 |
| **CSE 工具输出（After）** | **84** |

工具输出相对原始循环版节省 239 flops（74.0%）。结构：`var0 = 1 - 1.5*u²`
提取一次，每组方向对共享 `var0 + 4.5*uc²` 与 `3*uc`（符号规范化后正反方向
复用同一 `3*uc` 节点），每条 feq 只做 `±3*uc` 与权重乘；数值校验最大误差 ~1e-17。

### 真实头文件成本（`make cost`，由 csegen 测量）

FreeLB 头文件的成本由 **`csegen --cost`** 测量：它与生成 `.ur.h` 使用**同一条
管线**（per-struct `lowerVectors`、`CounterPropPass`、recombine 设置），因此数值
与生成代码严格一致。

> 早期 `make cost` 调用 `cse -c`：既没有按 struct kind 开启向量 lowering，又用
> `-r` 开启了 csegen 未启用的 recombine，导致 `moment/force` 的 After 多出一批
> 并不存在的“向量操作”。改为 `csegen --cost` 后，`vector-ops` 在 moment/force
> 上为 `0->0`，与标量化的 `.ur.h` 一致。（`cse -c` 仍是通用引擎入口，仅供夹具。）

`make cost`（`tools/cse/Makefile`，默认 `LATTICES="D2Q9 D3Q19"`）输出：

| 头文件 | lattice | Before | After | Saved | vector-ops |
|--------|:-------:|:------:|:-----:|:-----:|:----------:|
| `moment.h` | D2Q9 | 956 | 355 | 62.9% | 0→0 |
| `moment.h` | D3Q19 | 3113 | 781 | 74.9% | 0→0 |
| `equilibrium.h` | D2Q9 | 138 | 45 | 67.4% | 9→0 |
| `equilibrium.h` | D3Q19 | 328 | 91 | 72.3% | 19→0 |
| `force.h` | D2Q9 | 396 | 150 | 62.1% | 0→0 |
| `force.h` | D3Q19 | 1482 | 353 | 76.2% | 0→0 |

`equilibrium` 的 After=91（此前 89）。差的 2 来自 `const T&` 形参不再自动视为
只读根的正确性收紧，不是这次零恒等式改动引入的。

> 这组数字在 2026-10 一度整体退化近一倍（`moment` D3Q19 779 → 1477、`force`
> D3Q19 341 → 635）。根因是三条零恒等式要求被替换常量携带表达式的 int/float
> kind，而 `0 * cell[i]` 的操作数是 load、kind 无法证明，于是每个零方向分量都
> 原样留下。`CSEConfig::assumeFloatOperands`（FreeLB profile 开启，`-s` 关闭）
> 让"无法证明"按浮点处理，恢复了折叠；可证明的整型仍然优先，所以
> `(0 * k + 2) / 4` 依旧是整数除法。

## 构建

```bash
make          # 构建静态库、动态库与 bin/cse、bin/csegen
make test     # 自动构建后运行回归（tests/run_tests.sh）
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

`//@cse` 区域内的 `namespace` 会被完整处理：其中的 `using`、结构体与函数
都经过同一优化管线并原样包裹在 `namespace X { ... }` 中输出。
