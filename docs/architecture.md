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
- **Passes**：常量折叠 → 代数简化 → CSEPass → 表达式重组 → 值传播 → 死代码消除
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
    └── lattice_resolve.h/cpp  # latset::c/w 常量解析（硬编码查找表）
```

## 配置系统

CSE 工具通过 `CSEConfig` 结构支持多种项目配置：

```cpp
struct CSEConfig {
  std::function<bool(const Token&)> tokenFilter;  // token 过滤器
  bool simplifyBraceInit = false;                  // T{expr} → expr
};
```

### 使用方式

```cpp
// 通用 C++ 模式
cse::CSEConfig config;

// FreeLB/CUDA 模式
cse::CSEConfig config = cse::freelb::createFreeLBConfig();
// → tokenFilter = skipDoubleUnderscoreTokens, simplifyBraceInit = true
```

FreeLB 特定逻辑位于 `plugins/freelb/`：
- `cuda_skip.h/cpp` — 过滤 `__any__`、`__host__`、`__device__` 等 CUDA 注解
- `config.h` — FreeLB 默认配置工厂函数

## 优化管线

默认管线按以下顺序执行：

```
LoopUnroll → [LatticeResolve] → ConstantFold → AlgebraicSimplify → Reassociate
  → CSEPass → [ExprRecombine → AlgebraicSimplify] → ValueProp → DCE
```

| Pass | 功能 |
|------|------|
| **LoopUnroll** | `for (i=0; i<N; ++i)` 展开为 N 条语句，并内联循环体局部变量 |
| **LatticeResolve** | 插件 Pass：解析 `latset::c/w` 为常量/点积（FreeLB） |
| **ConstantFold** | 编译期计算常量表达式 |
| **AlgebraicSimplify** | 恒等消除 + 强度削减 + 交换律排序 + 常量乘法合并 + `(-a)*(-a)→a*a` |
| **Reassociate** | 加法项按全局出现频率重排，使对称语句共享不变前缀 |
| **CSEPass** | 跨语句公共子表达式提取 |
| **ExprRecombine** | 分配律提取公因子（-r 启用） |
| **ValueProp** | 内联简单赋值到使用处 |
| **DCE** | 删除未使用的变量声明和赋值 |

### 插件注入

`PassManager::createDefault(enableRecombine, resolvePass)` 接受一个可选的
项目专用 Pass，在循环展开之后、通用代数/CSE 之前运行。FreeLB 通过
`plugins/freelb/lattice_resolve` 提供该 Pass，核心 `src/` 不依赖任何项目。

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

- `forEachNode(root, f)` — 遍历 DAG 子树中的每个节点
- `countUses(root)` — 统计 StmtIR 树中每个变量名的使用次数
- `substitute(mod, root, name, replacement)` — 在 DAG 子树中替换变量
- `foldConst(mod, node)` — 常量折叠（BinaryOp 两个 Constant 操作数）

### 11. 构建系统

分离静态库 + 动态库 + 可执行文件：

```
bin/
├── libcse.a      # 静态库
├── libcse.so     # 动态库
└── cse           # 可执行文件（静态链接 libcse.a）
```

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

- 硬编码 D3Q19 / D2Q9 方向向量与权重查找表（与 `lattice_set.h` 一致）
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
| **重结合改变浮点舍入** | 加法项重排不满足 IEEE 结合律 | 对 LBM 核可接受，通用代码需谨慎 |
| **无通用 constexpr** | 仅模式匹配已知 lattice | 其他 constexpr 调用仍不透明 |

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
| `tests/test1.cpp` | 基本 CSE（11→10 flops, 9.1%） |
| `tests/test_all.cpp` | 多函数综合测试（51→50 flops, 2%） |
| `tests/equilibrium_d3q19.cpp` | FreeLB D3Q19 loop 版：展开 + 常量解析 + 重结合 |
| `tests/equilibrium_ref.cpp` | 手写展开版基线（89 flops） |
| `tests/verify_equilibrium.cpp` | 生成代码与参考实现数值一致性校验 |

### D3Q19 equilibrium 实测（cse -c，成本模型已计入循环次数）

| 版本 | FLOPs |
|------|:-----:|
| 原始循环版（Before） | 228 |
| 手写展开版 `equilibrium_ref.cpp` | 89 |
| **CSE 工具输出（After）** | **93** |

工具输出相对原始循环版节省 135 flops（59.2%），与手写版结构一致：
`var0 = 1 - 1.5*u²` 提取一次，每组方向对共享 `var0 + 4.5*uc²`，
每条 feq 只做 `±3*uc` 与权重乘；数值校验最大误差 ~1e-17。

## 构建

```bash
make          # 构建静态库、动态库和可执行文件
make clean    # 清理
./bin/cse input.cpp -c    # 分析 FLOP 成本
./bin/cse input.cpp -r    # 优化文件，启用表达式重组
```
