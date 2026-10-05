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
│   ├── statement.h       # IR 语句定义
│   ├── ir_module.h/cpp   # IR 模块（节点池 + 去重）
│   ├── ir_builder.h/cpp  # AST → IR 转换
│   └── ir_utils.h        # 工具函数（遍历、替换、折叠）
├── passes/
│   ├── pass.h            # Pass 基类
│   ├── pass_manager.h/cpp # Pass 管理器
│   ├── loop_unroll.h/cpp # 计数循环展开
│   ├── counter_prop.h/cpp # 计数器传播（插件注入的 post-algebra pass）
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
    └── cost_model.h/cpp  # FLOP 成本分析
plugins/
└── freelb/
    ├── cuda_skip.h/cpp   # __xx__ token 过滤器
    ├── config.h          # FreeLB 配置工厂函数
    ├── lattice_resolve.h/cpp  # latset::c/w 常量解析（硬编码查找表）
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

通用模式下优化保持行为等价，六条机制：

1. **效果/纯度**：调用默认视为有副作用，`createCall` 仅对纯调用去重；不纯调用不被合并、
   不跨语句提取。内置数学函数 + `isPureFunction` 白名单视为纯。任何"把两处出现折叠为一处"
   的重写，都要求被折叠的子表达式是纯的。
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
   两处扫描通过 `ir_utils.h:collectWrittenNames`（共用一份实现）把它记进写集合——
   只记根是刻意的：可能有别的指针指向同一对象，此时"不知道是否有别名"只能取保守答案。
   `VarDecl` 不记入（它引入的是新名字，不是覆盖已有值）。
   这类写在 IR 里是**结构化语句** `AssignIR`（`targetExpr` + `value`），不随
   `lowerVectors` 变化——早先它只在向量降级路径下才结构化，于是默认档下是一段不透明
   表达式，写集合漏掉了整类写。
4. **支配安全**：出现在 `if`/`else`/循环内的子表达式标记为 nested，不参与顶层提取，
   避免把条件执行的计算提升为无条件计算。
5. **作用域**：IRBuilder 维护作用域栈并对遮蔽变量 alpha-rename（如 `y__s1`），
   保证同名变量不跨作用域误合并。
6. **循环展开前提**：仅当循环体内每个局部声明都能被内联消除（初始化表达式为纯、
   非变量且该局部不被重新赋值）才展开；否则保留循环。这避免把同一局部变量
   跨迭代共享、进而被误当作循环不变量（例如 `uc` 的初始化含不纯调用时）。

另外，CSE 工具**总是原样打印 IR 的树**：同优先级的右子节点一律保留括号，因为浮点下
`a - (b - c)`、`a / (b * c)` 甚至 `a * (b * c)` 与去掉括号后的形式都不等价。

代数规则默认关闭，按**授权类型**分成四档（`CSEConfig`）：

| 开关 | 授权 |
|------|------|
| `assumeNumericCommutative` / `assumeNumericAssociative`（联合生效） | 交换/结合律重排、恒等消除 |
| `allowFpReassoc` | 浮点加法重排（`Reassociate`）、乘法链重排（`normalizeProduct`） |
| `allowUnsafeFpIdentities` | `x*0→0`、`x-x→0`、`0/x→0`、`x/x→1`、`x+0→x`、`0-x→-x`（在 `0`/`±inf`/`NaN`/符号零处改变结果） |

`-s`（保守档）全部关闭；默认档（FreeLB profile）全部打开。`-s` 与 `-r` 互不影响。

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
| **AlgebraicSimplify** | 恒等消除 + 强度削减 + 交换律排序 + 常量乘法合并 + `(-a)*(-a)→a*a`；FP 恒等与乘法链重排另有开关 |
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
DAGNode* IRModule::createBinaryOp(char op, DAGNode* lhs, DAGNode* rhs) {
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

**常量文本保留字面量拼写。** `createConst` 只在调用方未给出拼写时才自行格式化成不带
`.0` 的整数值；源码里的 `1e16` / `0.5` / `-0.0` 按原样发射。`numText` 不参与节点身份，
所以同一个常量节点可能同时出现在算术位置（`1.0 * x`）与下标位置（`feq[1]`）——
下标由 `codegen` 强制发射为整数字面量，否则会写出不编译的 `feq[1.0]`。

### 5. 代数简化 (AlgebraicSimplifyPass)

恒等消除 + 强度削减 + 叶子交换。整组规则受 `numeric_`（= 交换律 ∧ 结合律）门控；
其中 `a*0→0`、`a-a→0`、`0/a→0`、`a/a→1`、`a+0→a`、`0-a→-a` 六条另需
`allowUnsafeFpIdentities`，乘法链重排需 `allowFpReassoc`：

**恒等消除（`numeric_` 门控）：**
- `a * 1 → a`，`1 * a → a`
- `a - 0 → a`（精确），`a / 1 → a`
- `--a → a`（双重否定消除）
- `a + (-b) → a - b`，`a - (-b) → a + b`

**另需 `allowUnsafeFpIdentities`（在 `0`/`±inf`/`NaN`/符号零处改变结果）：**
- `a + 0 → a`，`0 + a → a`（`a = -0.0` 时和为 `+0.0`）
- `0 - a → -a`（`a = +0.0` 时差为 `+0.0`，而 `-a` 是 `-0.0`）
- `a * 0 → 0`，`0 * a → 0`
- `0 / a → 0`
- `a - a → 0`，`a / a → 1`

**强度削减：**
- `x * 2 → x + x`

**交换律排序：** 叶子节点按常量优先、变量名字典序排列，促进后续 CSE 匹配。

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

覆盖：`tests/fixtures/recombine.cpp`（`-r` 成本回归 18→21 flops，另有两项 FLOP 中性的
形状检查）+ `tests/verify/verify_recombine.cpp`（差分执行校验：乘法因子提取数值等价、
除法与整数除法语义不变、不纯调用求值两次）。

### 7. 值传播 (ValuePropPass)

内联简单初值到使用处，消除中间变量：

```cpp
double val = a;       // 初值是 Variable → 可内联
double t = val * val; // 内联后: double t = a * a;
```

`double val = a * x;` **不会**被内联——`isTrivial` 只接受
Constant / Variable / MemberAccess / ArrowAccess（后者基须为 Variable），
任何 `BinaryOp` 都会被拒绝（内联=复制表达式，只允许零成本的形式）。

**安全约束：**
- 只内联上述"零成本"表达式；
- 被内联的变量本身不得被重新赋值（`findReassigned` 预扫描）；
- **初值读取的变量不得在整个函数内被写过**（`initDepsStable`）。否则
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

声明可删的条件同样是两个：**既没被读也没被写**。`countUses` 只数出现次数（读），
对被写入过、却从没被读的名字一无所知——保留了一条 `unused = y++;` 之后，
若把 `double unused` 删掉，留下的语句就引用了一个不存在的变量。
`reads` 与 `written` 每轮重算，所以"删除赋值 → 下一轮释放声明"会自行收敛。

DCE 之后还跑一个 **CleanupPass** 做末段清扫：删除没被读**也没被写**的零初始化声明
（与 DCE 同一条 written 判定——只看"没被读"会把 `unused = y++;` 的声明删掉、留下
编译不过的悬空赋值，合并时曾真实发生）、删除自赋值（`u_value[i] = u_value[i];`）、
合并链式的声明赋值与赋值语句。`pruneBlock` 目前只清理函数体顶层 `Block`，
嵌套块的局限见"未完成"清单。

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
| `#include` / `#ifdef` 等预处理指令 | 已跳过 |
| `T{1}` 括号初始化 | 已支持（可配置简化） |
| `++k` / `--k` 前缀运算符 | 已支持 |
| `k++` / `k--` 后缀运算符 | 已支持（最长匹配成词，否则 `a++ + b` 与 `a + +b` 同流） |
| 一元 `+` | 已支持 |
| 科学计数法 / 浮点后缀字面量 | 已支持（`1e16` 曾lex成 `1` + 标识符 `e16`） |
| `f(void)` 空形参列表 | 已支持（`void` 是类型关键字，需在 `)` 前特判） |
| 字符串 / 字符字面量 | **不支持**（`"..."` 仍是 parse error；区域会被跳过并原样输出） |
| 模板函数调用 `latset::c<LatSet>(k)` | 已支持（作为不透明调用） |

### 10. IR 工具函数 (ir_utils.h / stmt_walk.h)

提供 pass 通用的工具函数，不侵入 IRModule：

- `forEachExpr(stmt, visit, includeLvalue=true)` —
  **语句拥有的表达式槽位**，就地遍历（`stmt_walk.h`）。`Assign` 贡献两个槽位：
  lvalue 与 value；引用语义允许改写。
- `forEachExprDeep(stmt, visit, includeLvalue=true)` — 同上，含嵌套语句。
- `countUses(root)` — 统计 StmtIR 树中每个变量名的使用次数（经 `forEachExprDeep`）
- `lvalueRoot(node)` — lvalue 的根变量名（`a[i].m` → `a`）
- `collectWrittenNames(stmt, out)` — 一条语句写了哪些名字（见「正确性与安全模型」第 3 条）
- `substitute(mod, root, name, replacement)` — 在 DAG 子树中替换变量
- `foldConst(mod, node)` — 常量折叠（BinaryOp 两个 Constant 操作数；除数为 0 时不折叠）
- `hasSideEffect(node)` — 丢弃这个值会不会丢掉副作用（不纯调用 / `++` `--` / `=`
  形式的赋值）。`hasImpureCall` 只看调用，DCE 曾因此删掉 `unused = y++;` 里的自增

**为什么要有 `stmt_walk.h`**：表达式槽位的清单原先在约十个 pass 里各写一遍
（`countUses`、DCE、五个重写型 pass、CSE 的三处收集/替换、cost model），
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
| **作用域实现较浅** | alpha-rename 处理遮蔽；`for` 内声明简化为同一作用域 | 复杂的声明/生命周期场景可能不准 |
| **数组复合赋值未建模** | `a[i] += x` 未展开为 `a[i] = a[i] + x` | 仅与变量 `x += y` 等价 |
| **无通用 constexpr** | 仅模式匹配已知 lattice | 其他 constexpr 调用仍不透明 |
| ~~引用型形参未纳入别名判据~~ | **已修**：`&` 与 `*`/`[` 一起进 `_pointerParams`（形参与引用型局部） | `f(const V& v, V& w)` 以 `f(x, x)` 调用时不再跨写复用 `v` 的 load（`ref_alias` 夹具钉住）；`noAlias` 仍是恢复共享的唯一出口 |
| **写集合只看 lvalue 的根** | `collectWrittenNames` 对元素/成员写只记根变量名 | 若两个不同根实际别名同一对象，检查会认为"没写"；`noAlias` 与"根名不同即不别名"是当前的全部依据 |
| **写集合不含声明** | `VarDecl` 不计入写集合 | 名字遮蔽已做 alpha-rename，故不会与旧绑定混淆；新增读写分析时注意这一点 |
| **FLOP 口径是「每条语句一次」** | `cost_model` 按**提及该节点的语句数**计数，而不是 DAG 节点数 | 它要近似的是**生成代码**的开销：DAG 已哈希去重，而 codegen 在每个使用点重印该节点。改成整函数去重会把 `for (i<4) a+=b;` 展开出的四条 `a = a + b;` 报成 1 flop（实为 4），所以不是缺陷；真正未解决的是同一条语句内重复的子表达式只计一次 |
| **常量文本不进身份** | `numText` / `symbol` 不是节点身份的一部分 | 同一个常量节点只有一份拼写，可能被算术位置与下标位置共用；下标由 codegen 保证整型 |

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

### 低优先级

| 方向 | 描述 | 状态 |
|------|------|------|
| **错误恢复** | 遇到 `;` 或 `}` 时同步，继续解析 | 待实现 |
| **CFG 支持** | 支持 break/continue/goto | 待实现 |

### 延后项（可选）

| 项 | 描述 | 状态 |
|----|------|------|
| **Cleanup 递归清理** | `CleanupPass::pruneBlock` 目前只清理函数体顶层 `Block`，不递归进入 `if`/嵌套块。向量 lowering 产生的自赋值（`u_value[i] = u_value[i];`）在 `LoopUnroll` 后位于顶层，已由本次修复覆盖；更深的嵌套场景需让 `pruneBlock` 递归 | 待实现 |
| **生成物自赋值回归断言** | 在 `tests/run_tests.sh` 的 csegen 冒烟中，对生成输出断言不存在 `a[i] = a[i];`（python 正则 backreference），防止 `CleanupPass` 自赋值删除逻辑回归 | 待实现 |

## 测试覆盖

| 测试 | 覆盖功能 |
|------|----------|
| `tests/fixtures/basic_cse.cpp` | 基本 CSE / 乘积链因式分解（10 flops after） |
| `tests/fixtures/features.cpp` | 多函数综合：结构体/方法、模板、箭头/成员访问、循环、分支（49 flops after；含 runtime-bound 循环，报告为下界） |
| `tests/fixtures/namespace_case.cpp` | `//@cse` 区域内 namespace 的函数/结构体被优化并输出（4 flops after） |
| `tests/fixtures/equilibrium_d3q19.cpp` | FreeLB D3Q19 loop 版：展开 + 常量解析 + 重结合（向量加权后 84 flops） |
| `tests/fixtures/safety_cases.cpp` | 正确性风险用例：不纯调用/load-store/分支/比较运算符/遮蔽（19 flops after） |
| `tests/fixtures/cost_nested.cpp` | 成本模型：三角 dim 循环、runtime-bound 循环（计一次并标记 unknownLoops）、向量加权、helper call（355 flops after，合并后实测；247 是依赖已删除的 `const`-shortcut 的旧值） |
| `tests/fixtures/cost_descending.cpp` | 成本模型：递减循环（`>=`+`--`、`>`+`-=k`）正确计数，runtime-bound 循环标记 unknownLoops（35 flops） |
| `tests/fixtures/recombine.cpp` | 表达式重组（`-r`）：乘法因子提取生效，除法/整数除法/不纯调用不被改写（24 flops after） |
| `tests/verify/verify_equilibrium.cpp` | D3Q19 生成代码与参考实现数值一致性 |
| `tests/verify/verify_safety.cpp` | 安全用例的差分执行校验 |
| `tests/verify/verify_recombine.cpp` | `-r` 输出的差分执行校验（数值等价 + 副作用出现次数） |
| `tests/fixtures/parens.cpp` | 括号保持：右子节点在等高优先级时必须保留括号（默认档 + `-s` 档，19 flops） |
| `tests/fixtures/store_aware.cpp` | 跨语句改写必须尊重写操作：CSE / ValueProp / `const` 别名；成员、箭头、元素写；只写不读的局部必须活过 DCE；共享索引要两侧同时改写（19 flops，`-s` 档） |
| `tests/fixtures/float_identities.cpp` | `-s` 档必须保留 0 / ±inf / NaN 处的 IEEE 语义（4 flops） |
| `tests/verify/verify_parens.cpp` | 括号保持的差分执行校验（覆盖 `-` `/` `*` `%` 的各种右子节点） |
| `tests/verify/verify_store_aware.cpp` | 跨 store 重写的差分执行校验（含**真别名**调用） |
| `tests/fixtures/ref_alias.cpp` | 引用形参与指针同一条别名规则：`f(const V& v, V& w)` 以同一对象调用时不得跨写复用 `v` 的 load（1 flops，`-s` 档） |
| `tests/verify/verify_ref_alias.cpp` | 真别名调用 `f(x, x)` 的数值校验（修复前得 2.0，应为 10.0） |
| `tests/verify/verify_float_identities.cpp` | 特殊值下的数值校验（NaN 是否仍然产生） |
| `tests/verify/verify_config.cpp` | 库层 `CSEConfig` 契约（CLI 无法隔离的新开关） |
| `tests/fixtures/comment_braces.cpp` | 区域提取：注释里的 `{` `}` 不参与括号计数（默认档 + `-s` 档，4 flops） |
| `tests/fixtures/dead_store_effects.cpp` | DCE 保留带副作用的死存储；纯死存储仍删除（3 flops + 三条形状检查） |
| `tests/fixtures/constant_edges.cpp` | 除零不折叠、`-0.0` 与 `1e16` 拼写保留、共享字面量的下标仍为整数（6 / `-s` 7 flops + 三条文本检查） |
| `tests/fixtures/void_param.cpp` | `f(void)` 可解析，普通形参列表不受影响（4 flops） |
| `tests/verify/verify_comment_braces.cpp` | 注释含括号的区域的数值校验 |
| `tests/verify/verify_dead_store_effects.cpp` | 自增/自减、全局写、不纯调用、纯死存储的数值校验 |
| `tests/verify/verify_constant_edges.cpp` | 除零得 inf、`-0.0` 的符号、共享字面量下标的数值校验 |
| `tests/verify/verify_void_param.cpp` | `(void)` 三种形态的数值校验 |
| `tests/verify/verify_builder_guards.cpp` | 库层：空语句槽位必须被拒绝为 `CSEError` 而不是解引用空指针 |
| `tests/verify/check_lattice.py` | 引擎 latset 表 vs FreeLB `lattice_set.h` 防漂移 |
| `tests/csegen/{equilibrium,force,moment}.h` | `csegen` `.ur.h` 生成冒烟（Cell/TLatSet/TLatSetD/CellType 各形态） |

> 所有工具产物（`*.cse`、`*.ur.h`、验证器可执行文件）写入临时目录，源码树不被修改。
> 数值正确性以夹具 + 验证器成对覆盖（equilibrium、safety、recombine、parens、store_aware、ref_alias、
> float_identities、mixed_ops、write_visibility、effect_duplication、frontend_forms、
> comment_braces、dead_store_effects、constant_edges、void_param），而非 golden-diff。
> 注意 **FLOP 回归对某些缺陷无效**：括号丢失不改变 flops，`a*x ± a` 的提取是 FLOP 中性的，
> 跨成员写的错误共享也恰好省下同样的 flops（store_aware 修复前后是同一个数），
> `y++` 与 `-0.0` 更是完全不进 flops——这几类只能靠数值验证器或生成文本的形状检查，
> 因此 `run_tests.sh` 里另有一组 grep 形状检查（重组形态、强度削减、死存储、常量发射、
> store 索引）。
> FreeLB 侧 `verify_*.py` 是**数值**校验：解析两个文件、按公式求值再逐个比较，
> 不做文本比对（所以重命名 `_cse_*` 这类内部标识符它发现不了，字面量拼写变化也不影响它）。
> 入口是 `make test`（`tests/run_tests.sh`）：FLOP 代价回归（默认档 + `-r` 档 + `-s` 档）→
> 形状检查 → 数值校验 → 库层契约（`verify_config` + `verify_builder_guards`）→ `csegen`
> 冒烟；`check_lattice.py` 与 FreeLB 侧 `verify_*.py` 仅在存在 FreeLB checkout
> （`FREELB=` 或 `~/FreeLB`）时运行，否则跳过。

### 成本模型（重设计）

成本模型现在按 **标量 FLOP、向量按 lane 加权** 计算：向量算子的代价为其 lane
数，向量点积为 `2d-1`，纯 helper/intrinsic 调用由 FreeLB 插件按名给定代价
（`getnorm2 -> 2d-1` 等），未知调用与无法解析的循环会被显式计数
（`unknownLoops` / `unmodeledCalls`）。共享子表达式按 **唯一 DAG 节点** 计一次
（与生成的 CSE 临时变量一致），循环体按执行次数缩放（三角 `for (b=a; b<d; ++b)`
嵌套精确计数）。详见 `docs/cost_model_redesign.md`。

### D3Q19 equilibrium 实测（cse -c，向量加权 + 唯一节点）

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
| `moment.h` | D2Q9 | 958 | 339 | 64.6% | 0→0 |
| `moment.h` | D3Q19 | 3115 | 749 | 76.0% | 0→0 |
| `equilibrium.h` | D2Q9 | 138 | 43 | 68.8% | 1→0 |
| `equilibrium.h` | D3Q19 | 328 | 89 | 72.9% | 1→0 |
| `force.h` | D2Q9 | 360 | 143 | 60.3% | 0→0 |
| `force.h` | D3Q19 | 1254 | 341 | 72.8% | 0→0 |

`equilibrium` 的 After=89 与手写参考基线一致。

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
