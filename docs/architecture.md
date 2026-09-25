# CSE 优化工具架构文档

## 概述

CSE（Common Subexpression Elimination）优化工具是一个基于 LLVM 风格三阶段架构的 C++ 代码优化器。它分析标记了 `//@cse` 注释的代码区域，识别公共子表达式并进行优化。

## 架构

### 三阶段流水线

```
源代码 → Frontend (Lexer/Parser/AST) → IR (DAG-based) → Passes → Backend (CodeGen)
```

每一阶段独立运作，通过明确的数据结构连接：

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
  bool allowFpReassoc = false;                    // 允许浮点重结合
  bool noAlias = false;                           // 假设不同指针参数不别名
  std::function<bool(const std::string&)> isPureFunction;  // 纯函数判定

  // 项目钩子（通用扩展点，`src/` 内无 FreeLB 语义）
  std::function<DAGNode*(IRModule&, const std::string&)> resolveName;  // 名字→常量
  bool lowerVectors;                              // 把向量类型降级为分量标量
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

FreeLB 特定逻辑位于 `plugins/freelb/`：
- `cuda_skip.h/cpp` — 过滤 `__any__`、`__host__`、`__device__` 等 CUDA 注解
- `config.h` — FreeLB 默认配置工厂函数 + 纯函数注册
- `lattice_resolve.h/cpp` — lattice 常量/点积解析
- `ur_emit.h/cpp` + `ur_emit_main.cpp` — `.ur.h` 生成（`bin/csegen`）
- `cse_main.cpp` — 通用 CLI 驱动（`bin/cse`）

## 正确性与安全模型

通用模式下优化保持行为等价，五条机制：

1. **效果/纯度**：调用默认视为有副作用，`createCall` 仅对纯调用去重；不纯调用不被合并、
   不跨语句提取。内置数学函数 + `isPureFunction` 白名单视为纯。
2. **内存屏障**：IRBuilder 预扫描得到只读根（`const` 参数、未被写且未传入调用的变量）；
   仅只读根的 load 可去重/跨语句共享，可变/未知根的 load 每次独立，避免跨 store 复用。
3. **支配安全**：出现在 `if`/`else`/循环内的子表达式标记为 nested，不参与顶层提取，
   避免把条件执行的计算提升为无条件计算。
4. **作用域**：IRBuilder 维护作用域栈并对遮蔽变量 alpha-rename（如 `y__s1`），
   保证同名变量不跨作用域误合并。
5. **循环展开前提**：仅当循环体内每个局部声明都能被内联消除（初始化表达式为纯、
   非变量且该局部不被重新赋值）才展开；否则保留循环。这避免把同一局部变量
   跨迭代共享、进而被误当作循环不变量（例如 `uc` 的初始化含不纯调用时）。

代数规则（恒等消除、交换/结合律、`Reassociate`）默认关闭，仅在
`assumeNumericCommutative/Associative` 打开时启用；浮点重结合另有 `allowFpReassoc`。

## 优化管线

默认管线按以下顺序执行（方括号为可选/受配置门控的环节）：

```
LoopUnroll → [Resolve] → ConstantFold → AlgebraicSimplify → [PostAlgebra]
  → [Reassociate] → CSEPass → [ExprRecombine → AlgebraicSimplify] → ValueProp → DCE
```

| Pass | 功能 |
|------|------|
| **LoopUnroll** | `for (i=0; i<N; ++i)` 展开为 N 条语句，并内联循环体局部变量 |
| **Resolve** | 插件 Pass（`resolvePass`）：解析 `latset::c/w` 为常量/点积（FreeLB） |
| **ConstantFold** | 编译期计算常量表达式 |
| **AlgebraicSimplify** | 恒等消除 + 强度削减 + 交换律排序 + 常量乘法合并 + `(-a)*(-a)→a*a` |
| **PostAlgebra** | 插件 Pass 槽位（`postAlgebraPass`），FreeLB 注入 **CounterProp**：直线计数器解析（`tensor[i]→tensor[0]`）、降级向量局部索引（`unew[1]→unew_1`）、常量条件折叠 |
| **Reassociate** | 加法项按全局出现频率重排，使对称语句共享不变前缀（需 `assumeNumericAssociative` **且** `allowFpReassoc`） |
| **CSEPass** | 跨语句公共子表达式提取 |
| **ExprRecombine** | 分配律提取公因子（-r 启用） |
| **ValueProp** | 内联简单赋值到使用处 |
| **DCE** | 删除未使用的变量声明和赋值 |

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
    candidate->recomputeHash();
    return findExistingNode(candidate);  // 结构相同则复用已有节点
}
```

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

### 5. 代数简化 (AlgebraicSimplifyPass)

恒等消除 + 强度削减 + 叶子交换：

**恒等消除：**
- `a * 1 → a`，`1 * a → a`
- `a + 0 → a`，`0 + a → a`
- `a * 0 → 0`，`0 * a → 0`
- `a - 0 → a`，`a / 1 → a`
- `0 / a → 0`
- `a - a → 0`，`a / a → 1`
- `--a → a`（双重否定消除）
- `a + (-b) → a - b`，`a - (-b) → a + b`

**强度削减：**
- `x * 2 → x + x`

**交换律排序：** 叶子节点按常量优先、变量名字典序排列，促进后续 CSE 匹配。

### 6. 表达式重组 (ExprRecombinePass)

分配律识别，支持 `+`、`-`、`*`、`/` 运算符：

- `a * x + a * y → a * (x + y)`
- `a * x - a * y → a * (x - y)`
- `a * x + b * x → (a + b) * x`
- `a * x + a → a * (x + 1)`
- `a * x - a → a * (x - 1)`
- 跨匹配：`a * x + b * y` 尝试所有组合

### 7. 值传播 (ValuePropPass)

内联简单赋值到使用处，消除中间变量：

```cpp
double val = a * x;   // val 是简单赋值（Trivial expression）
double t = val * val; // 内联后: double t = a * x * a * x;
```

**安全约束：**
- 只内联"简单"表达式：Constant、Variable、MemberAccess、ArrowAccess
- 不内联被重新赋值的变量（通过 `findReassigned` 预扫描）
- 不内联函数参数

### 8. 死代码消除 (DCEPass)

删除未使用的变量声明和赋值：

```cpp
double unused = x * y;  // 使用次数为 0，删除
double t = a + b;       // 使用次数 > 0，保留
return t;
```

迭代至不动点，因为删除可能暴露新的死代码。

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
| 模板函数调用 `latset::c<LatSet>(k)` | 已支持（作为不透明调用） |

### 10. IR 工具函数 (ir_utils.h)

提供 pass 通用的工具函数，不侵入 IRModule：

- `countUses(root)` — 统计 StmtIR 树中每个变量名的使用次数
- `substitute(mod, root, name, replacement)` — 在 DAG 子树中替换变量
- `foldConst(mod, node)` — 常量折叠（BinaryOp 两个 Constant 操作数）

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
| **无错误恢复** | 解析错误抛异常后终止 | 一次只能报告一个错误 |
| **仅处理标记区域** | 只解析 `//@cse` 标记的代码 | 无法跨区域优化 |
| **作用域实现较浅** | alpha-rename 处理遮蔽；`for` 内声明简化为同一作用域 | 复杂的声明/生命周期场景可能不准 |
| **数组复合赋值未建模** | `a[i] += x` 未展开为 `a[i] = a[i] + x` | 仅与变量 `x += y` 等价 |
| **无通用 constexpr** | 仅模式匹配已知 lattice | 其他 constexpr 调用仍不透明 |

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

## 测试覆盖

| 测试 | 覆盖功能 |
|------|----------|
| `tests/fixtures/basic_cse.cpp` | 基本 CSE / 乘积链因式分解（11→10 flops） |
| `tests/fixtures/features.cpp` | 多函数综合：结构体/方法、模板、箭头/成员访问、循环、分支（51→50 flops） |
| `tests/fixtures/namespace_case.cpp` | `//@cse` 区域内 namespace 的函数/结构体被优化并输出（6→4 flops） |
| `tests/fixtures/equilibrium_d3q19.cpp` | FreeLB D3Q19 loop 版：展开 + 常量解析 + 重结合（228→84 flops） |
| `tests/fixtures/safety_cases.cpp` | 正确性风险用例：不纯调用/load-store/分支/比较运算符/遮蔽（20→20，验证不劣化） |
| `tests/verify/verify_equilibrium.cpp` | D3Q19 生成代码与参考实现数值一致性 |
| `tests/verify/verify_safety.cpp` | 安全用例的差分执行校验 |
| `tests/verify/check_lattice.py` | 引擎 latset 表 vs FreeLB `lattice_set.h` 防漂移 |
| `tests/csegen/{equilibrium,force,moment}.h` | `csegen` `.ur.h` 生成冒烟（Cell/TLatSet/TLatSetD/CellType 各形态） |

> 所有工具产物（`*.cse`、`*.ur.h`、验证器可执行文件）写入临时目录，源码树不被修改。
> 数值正确性以夹具 + 验证器成对覆盖（equilibrium、safety），而非 golden-diff。
> 入口是 `make test`（`tests/run_tests.sh`）：FLOP 代价回归 → 数值校验 → `csegen`
> 冒烟；`check_lattice.py` 与 FreeLB 侧 `verify_*.py` 仅在存在 FreeLB checkout
> （`FREELB=` 或 `~/FreeLB`）时运行，否则跳过。

### D3Q19 equilibrium 实测（cse -c，成本模型已计入循环次数）

| 版本 | FLOPs |
|------|:-----:|
| 原始循环版（Before） | 228 |
| 手写展开版参考（历史基线） | 89 |
| **CSE 工具输出（After）** | **84** |

工具输出相对原始循环版节省 144 flops（63.2%）。结构：`var0 = 1 - 1.5*u²`
提取一次，每组方向对共享 `var0 + 4.5*uc²` 与 `3*uc`（符号规范化后正反方向
复用同一 `3*uc` 节点），每条 feq 只做 `±3*uc` 与权重乘；数值校验最大误差 ~1e-17。

> 注：手写版内联了 `u²`（7 flops），而工具把 `u.getnorm2()` 视为不透明的
> Call（计 0 flops，隐藏 5 flops）。若按同一口径计入，工具与手写版均为 89。

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
