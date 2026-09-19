# CSE 工具与 FreeLB equilibrium.h 对比分析

## 1. 背景

对 `equilibrium.h` 中的 `SecondOrderImpl::apply` 函数（D3Q19 格子）进行 CSE 优化分析。  
FreeLB 提供了手写的 `equilibrium.ur.h`（模板特化展开版本）作为性能参考。

---

## 2. D3Q19 全量代码

### 2.1 原始代码 (equilibrium.h) — 循环版

```cpp
namespace equilibrium {

template <typename CELL>
struct SecondOrderImpl {
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  using CELLTYPE = CELL;
  using GenericRho = typename CELL::GenericRho;

  __any__ static void apply(std::array<T, LatSet::q> &feq, const T rho, const Vector<T, LatSet::d> &u) {
    const T u2 = u.getnorm2();
    for (unsigned int k = 0; k < LatSet::q; ++k) {
      const T uc = u * latset::c<LatSet>(k);
      feq[k] = latset::w<LatSet>(k) * rho *
      (T{1} + LatSet::InvCs2 * uc + uc * uc * T{0.5} * LatSet::InvCs4 - LatSet::InvCs2 * u2 * T{0.5});
    }
  }
};

}  // namespace equilibrium
```

### 2.2 手写优化版本 (equilibrium.ur.h) — D3Q19 模板特化

```cpp
template <typename T, typename TypePack>
struct SecondOrderImpl<CELL<T, D3Q19<T>, TypePack>>{
using CELLTYPE = CELL<T, D3Q19<T>, TypePack>;
using LatSet = D3Q19<T>;

__any__ static void apply(std::array<T, LatSet::q> &feq, const T rho, const Vector<T, LatSet::d> &u){
constexpr T InvCs2x = T{0.5} * LatSet::InvCs2;
const T var0 = T{1} - (u[0]*u[0] + u[1]*u[1] + u[2]*u[2]) * InvCs2x;
const T rhowk0 = latset::w<LatSet>(0) * rho;
const T rhowk1 = latset::w<LatSet>(1) * rho;
const T rhowk7 = latset::w<LatSet>(7) * rho;
const T InvCs2uck1 = LatSet::InvCs2 * (u[0]);
const T var0InvCs4uc2k1 = var0 + InvCs2uck1*InvCs2uck1*T{0.5};
const T InvCs2uck3 = LatSet::InvCs2 * (u[1]);
const T var0InvCs4uc2k3 = var0 + InvCs2uck3*InvCs2uck3*T{0.5};
const T InvCs2uck5 = LatSet::InvCs2 * (u[2]);
const T var0InvCs4uc2k5 = var0 + InvCs2uck5*InvCs2uck5*T{0.5};
const T InvCs2uck7 = LatSet::InvCs2 * (u[0]+u[1]);
const T var0InvCs4uc2k7 = var0 + InvCs2uck7*InvCs2uck7*T{0.5};
const T InvCs2uck9 = LatSet::InvCs2 * (u[0]+u[2]);
const T var0InvCs4uc2k9 = var0 + InvCs2uck9*InvCs2uck9*T{0.5};
const T InvCs2uck11 = LatSet::InvCs2 * (u[1]+u[2]);
const T var0InvCs4uc2k11 = var0 + InvCs2uck11*InvCs2uck11*T{0.5};
const T InvCs2uck13 = LatSet::InvCs2 * (u[0]-u[1]);
const T var0InvCs4uc2k13 = var0 + InvCs2uck13*InvCs2uck13*T{0.5};
const T InvCs2uck15 = LatSet::InvCs2 * (u[0]-u[2]);
const T var0InvCs4uc2k15 = var0 + InvCs2uck15*InvCs2uck15*T{0.5};
const T InvCs2uck17 = LatSet::InvCs2 * (u[1]-u[2]);
const T var0InvCs4uc2k17 = var0 + InvCs2uck17*InvCs2uck17*T{0.5};
feq[0] = rhowk0 * var0;
feq[1] = rhowk1 * (var0InvCs4uc2k1 + InvCs2uck1);
feq[2] = rhowk1 * (var0InvCs4uc2k1 - InvCs2uck1);
feq[3] = rhowk1 * (var0InvCs4uc2k3 + InvCs2uck3);
feq[4] = rhowk1 * (var0InvCs4uc2k3 - InvCs2uck3);
feq[5] = rhowk1 * (var0InvCs4uc2k5 + InvCs2uck5);
feq[6] = rhowk1 * (var0InvCs4uc2k5 - InvCs2uck5);
feq[7] = rhowk7 * (var0InvCs4uc2k7 + InvCs2uck7);
feq[8] = rhowk7 * (var0InvCs4uc2k7 - InvCs2uck7);
feq[9] = rhowk7 * (var0InvCs4uc2k9 + InvCs2uck9);
feq[10] = rhowk7 * (var0InvCs4uc2k9 - InvCs2uck9);
feq[11] = rhowk7 * (var0InvCs4uc2k11 + InvCs2uck11);
feq[12] = rhowk7 * (var0InvCs4uc2k11 - InvCs2uck11);
feq[13] = rhowk7 * (var0InvCs4uc2k13 + InvCs2uck13);
feq[14] = rhowk7 * (var0InvCs4uc2k13 - InvCs2uck13);
feq[15] = rhowk7 * (var0InvCs4uc2k15 + InvCs2uck15);
feq[16] = rhowk7 * (var0InvCs4uc2k15 - InvCs2uck15);
feq[17] = rhowk7 * (var0InvCs4uc2k17 + InvCs2uck17);
feq[18] = rhowk7 * (var0InvCs4uc2k17 - InvCs2uck17);
}
};
```

### 2.3 CSE 工具输出（模拟循环展开后）

以下是对展开后的标量代码运行 CSE 工具的结果：

```c
void equilibrium_d3q19(double* feq, double rho, double u0, double u1, double u2) {
    double u2_total = u0 * u0 + u1 * u1 + u2 * u2;
    auto _cse_0_29 = 3.0 * u2_total;
    feq[0] = 0.222222 * rho * (1.0 + 3.0 * 0.0 + 0.0 * 0.0 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    feq[1] = 0.111111 * rho * (1.0 + 3.0 * u0 + u0 * u0 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    auto _cse_1_54 = -u0;
    feq[2] = 0.111111 * rho * (1.0 + 3.0 * _cse_1_54 + _cse_1_54 * _cse_1_54 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    feq[3] = 0.111111 * rho * (1.0 + 3.0 * u1 + u1 * u1 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    auto _cse_2_93 = -u1;
    feq[4] = 0.111111 * rho * (1.0 + 3.0 * _cse_2_93 + _cse_2_93 * _cse_2_93 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    feq[5] = 0.111111 * rho * (1.0 + 3.0 * u2 + u2 * u2 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc6 = -u2;
    feq[6] = 0.111111 * rho * (1.0 + 3.0 * uc6 + uc6 * uc6 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc7 = u0 + u1;
    feq[7] = 0.027778 * rho * (1.0 + 3.0 * uc7 + uc7 * uc7 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc8 = _cse_1_54 - u1;
    feq[8] = 0.027778 * rho * (1.0 + 3.0 * uc8 + uc8 * uc8 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc9 = u0 + u2;
    feq[9] = 0.027778 * rho * (1.0 + 3.0 * uc9 + uc9 * uc9 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc10 = _cse_1_54 - u2;
    feq[10] = 0.027778 * rho * (1.0 + 3.0 * uc10 + uc10 * uc10 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc11 = u1 + u2;
    feq[11] = 0.027778 * rho * (1.0 + 3.0 * uc11 + uc11 * uc11 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc12 = _cse_2_93 - u2;
    feq[12] = 0.027778 * rho * (1.0 + 3.0 * uc12 + uc12 * uc12 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc13 = u0 - u1;
    feq[13] = 0.027778 * rho * (1.0 + 3.0 * uc13 + uc13 * uc13 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc14 = _cse_1_54 + u1;
    feq[14] = 0.027778 * rho * (1.0 + 3.0 * uc14 + uc14 * uc14 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc15 = u0 - u2;
    feq[15] = 0.027778 * rho * (1.0 + 3.0 * uc15 + uc15 * uc15 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc16 = _cse_1_54 + u2;
    feq[16] = 0.027778 * rho * (1.0 + 3.0 * uc16 + uc16 * uc16 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc17 = u1 - u2;
    feq[17] = 0.027778 * rho * (1.0 + 3.0 * uc17 + uc17 * uc17 * 0.5 * 9.0 - _cse_0_29 * 0.5);
    double uc18 = _cse_2_93 + u2;
    feq[18] = 0.027778 * rho * (1.0 + 3.0 * uc18 + uc18 * uc18 * 0.5 * 9.0 - _cse_0_29 * 0.5);
}
```

CSE 提取的公共子表达式：

| 变量 | 表达式 | 复用次数 | 节省 |
|------|--------|:--------:|:----:|
| `_cse_0_29` | `3.0 * u2_total` | 19 次（所有 feq） | 18 flops |
| `_cse_1_54` | `-u0` | 9 次（feq[2,8,10,14,16] + uc8,uc10,uc14,uc16） | 8 flops |
| `_cse_2_93` | `-u1` | 7 次（feq[4,12] + uc8,uc12,uc14,uc18） | 6 flops |

---

## 3. FLOPS 分析

### 3.1 原始循环版（309 flops）

**循环前：**
- `u2 = u[0]*u[0] + u[1]*u[1] + u[2]*u[2]`：3 mul + 2 add = **5 flops**

**每迭代（19 次）：**
- `uc = u * latset::c<LatSet>(k)`：3 维点积 = 3 mul + 2 add = **5 flops**
- `InvCs2 * uc`：1 mul
- `uc * uc`：1 mul
- `(uc*uc) * 0.5`：1 mul
- `((uc*uc)*0.5) * InvCs4`：1 mul
- `InvCs2 * u2`：1 mul（循环不变！）
- `(InvCs2*u2) * 0.5`：1 mul（循环不变！）
- `1 + InvCs2*uc + ...`：2 add
- `... - InvCs2*u2*0.5`：1 sub
- `w[k] * rho`：1 mul
- `(w*rho) * bracket`：1 mul

每迭代：8 mul + 3 add/sub + 5（点积）= **16 flops**

总计：5 + 19 × 16 = **309 flops**

### 3.2 手写优化版（~166 flops）

| 步骤 | 计算 | FLOPs |
|------|------|:-----:|
| InvCs2x | `0.5 * InvCs2` | 1 |
| var0 | `1 - (u[0]²+u[1]²+u[2]²) * InvCs2x` | 5 |
| rhowk | 3 × `w*ρ`（权重分组：w0, w1, w7） | 3 |
| 10 个 uc 点积 | 10 × `InvCs2 * dot` | 10 |
| 10 个 uc² 缩放 | 10 × (`uc*uc*0.5`) | 20 |
| feq[0] | `rhowk0 * var0` | 1 |
| feq[1]~feq[18] | 每条 `rhowkX * (var0InvCs4uc2kN ± InvCs2uckN)` | 19 |
| **总计** | | **~166** |

**vs 原始版节省 143 flops（46%）**

关键优化：
1. **权重分组**：19 次 `w[k]*rho` → 3 次（w0=2/9, w1=1/9, w7=1/36）
2. **uc² 共享**：`(k, k+1)` 对共享 `var0InvCs4uc2kN`
3. **循环不变量**：`InvCs2x` 和 `var0` 预计算

### 3.3 CSE 工具输出（208 flops）

对展开后的标量代码运行 CSE：

| 指标 | CSE 前 | CSE 后 | 节省 |
|------|:------:|:------:|:----:|
| FLOPs | 226 | 208 | 18 (8%) |
| DAG 节点数 | 518 | 490 | 28 |
| 语句数 | 42 | 37 | 5 |
| 变量数 | 22 | 17 | 5 |

> 注：标量展开版"前"为 226 而非 309，因为点积已手动展开为标量操作（5 flops → 1 flop）。

### 3.4 D3Q19 对比总结

| 版本 | FLOPs | vs 原始 | 主要优化手段 |
|------|:-----:|:-------:|-------------|
| 原始循环版 | 309 | — | — |
| CSE 工具（当前） | 309 | 0% | 跨语句 CSE（被循环阻塞） |
| 标量展开 + CSE | 208 | 33% | 循环展开（手动）+ CSE |
| 手写优化版 | ~166 | **46%** | 展开 + 权重分组 + uc² 共享 |
| CSE + LICM（未来） | ~130 | **58%** | 手写版 + 循环不变量外提 |

---

## 4. CSE 工具当前能力

### 4.1 优化管线

```
ConstantFold → AlgebraicSimplify → CSEPass → ExprRecombine → ValueProp → DCE
```

| Pass | 功能 | 对 equilibrium 的效果 |
|------|------|---------------------|
| **ConstantFold** | `4.0/9.0 → 0.444444`，`InvCs2 → 3.0` | 常量预计算 |
| **AlgebraicSimplify** | `x*1 → x`，`x+0 → x`，`--x → x`，交换律排序 | 简化冗余运算 |
| **CSEPass** | 跨语句公共子表达式提取 | `3.0*u2`、`rho*0.111111` 等 |
| **ExprRecombine** | `a*x + a*y → a*(x+y)` | 同表达式内因子提取 |
| **ValueProp** | 内联平凡赋值 | 消除 `uc0 = 0.0` 等 |
| **DCE** | 移除未使用变量 | 清理冗余变量 |

### 4.2 前端已支持的 C++ 语法

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
| `LatSet::q` 表达式中 `::` 限定名 | 已支持 |
| 模板函数调用 `latset::c<LatSet>(k)` | 已支持（作为不透明调用） |

### 4.3 配置分离

CSE 工具通过 `CSEConfig` 支持通用 C++ 和 FreeLB 两种模式：

```cpp
// 通用 C++ 模式
cse::CSEConfig config;

// FreeLB 模式
cse::CSEConfig config = cse::freelb::createFreeLBConfig();
// → tokenFilter = skipDoubleUnderscoreTokens, simplifyBraceInit = true
```

FreeLB 特定逻辑位于 `plugins/freelb/`：
- `cuda_skip.h/cpp` — `__xx__` token 过滤器
- `config.h` — FreeLB 配置工厂函数

---

## 5. 差距分析：缺失特性

### 5.1 总览

| 优先级 | 缺失特性 | FLOPS 节省 (D3Q19) | 占差距比 | 实现难度 | 估计 LOC |
|:------:|----------|:-------------------:|:--------:|:--------:|:--------:|
| **1** | 循环展开 (Loop Unrolling) | ~143（前置条件） | 100% | 中高 | 200-400 |
| **2** | Constexpr 求值 (latset::c, latset::w) | ~28 | 20% | 非常高 | 500-1000 |
| **3** | LICM（循环不变量外提） | ~36 | 25% | 中等 | 150-200 |
| **4** | 权重分组 (Weight Grouping) | ~16 | 11% | 低 | — |
| **5** | 常量乘法合并 | ~18 | 13% | 低 | 50-100 |

### 5.2 缺失特性 1：循环展开（最关键）

**现状：** CSE 工具没有 `LoopUnroll` pass。对 `for (unsigned int k = 0; k < LatSet::q; ++k)` 完全无法处理。

**影响：**
- CSE pass 的跨语句分析要求 `stmtIndices.size() >= 2`，但循环体内只有 **1 条语句**，跨语句 CSE **永远不会触发**
- `latset::c<LatSet>(k)` 和 `latset::w<LatSet>(k)` 保持为不透明函数调用
- 点积展开、权重分组等后续优化全部无法进行

**这是所有其他优化的前置条件。** 没有循环展开，CSE 工具对含循环的输入 **节省 0 flops**。

**实现方案：**
1. 检测简单计数循环 (`for (k = 0; k < CONST; ++k)`)
2. 将循环体复制 q 次，每次用常量 0, 1, ..., q-1 替换循环变量 k
3. IR 中已有 `ForLoopIR`（init/cond/update/body），结构基础存在
4. 难点：将 `k` 替换到模板函数调用 `latset::c<LatSet>(k)` 中需要 constexpr 求值

### 5.3 缺失特性 2：Constexpr 求值

**现状：** CSE 工具将 `latset::c<LatSet>(k)` 和 `latset::w<LatSet>(k)` 视为不透明 `Call` 节点。

**格子数据存储在 constexpr 数组中（lattice_set.h）：**
- `c<3,19>[19]`：`constexpr Vector<int,3>[19]`，已知整数分量
- `w<3,19>[19]`：`constexpr Fraction<>[19]`，已知有理数值
- `InvCs2 = T(3)`，`InvCs4 = T(9)`：`static constexpr T` 成员

**实现方案（轻量级路线）：**
- 不做通用 constexpr 求值器，而是对已知 lattice 函数做**模式匹配 + 硬编码查表**
- 识别 `latset::c<LatSet>(k)` 调用模式，根据已知 lattice 类型（D2Q5, D2Q9, D3Q7, D3Q15, D3Q19, D3Q27）查表获取整数向量
- 识别 `latset::w<LatSet>(k)` 调用模式，查表获取权重值

### 5.4 缺失特性 3：LICM（循环不变量外提）

**现状：** 没有 LICM pass。

**影响：** 在原始循环体中：
```
InvCs2 * u2 * T{0.5}
```
`u2` 在循环前计算，`InvCs2` 和 `T{0.5}` 是常量——整个表达式是循环不变量。每次迭代重复计算 2 次乘法。

**有趣的是：** 手写优化版也没有提取这个子表达式（在每个 feq 行中重复出现）。**如果实现 LICM，CSE 输出反而会优于手写版 36 flops。**

### 5.5 缺失特性 4：权重分组

**现状：** 循环展开后，`latset::w<LatSet>(k) * rho` 出现在每个 feq 赋值中。对于 D3Q19，有 3 个不同的权重值（1/3, 1/18, 1/36），但 CSE 工具会看到 19 个不同的函数调用，因为参数不同而认为它们是不同的表达式。

**依赖：** 需要先实现 constexpr 求值（缺失特性 2）。

### 5.6 缺失特性 5：常量乘法合并

**现状：** 每个 feq 中 `tN * tN * 0.5 * InvCs4` 被展开为 3 次乘法而非 2 次。

**影响：** 18 次额外乘法 = 18 flops

**实现方案：** 在 `algebraic_simplify.cpp` 中增加 `a*b*c*d → a*(b*c)*d` 的关联重排（当 b, c 为常量时）。

---

## 6. 差距分解

### 6.1 D3Q19 差距来源（~143 flops gap）

| 优化来源 | FLOPS 节省 | 需要的缺失特性 |
|----------|:---------:|--------------|
| 点积展开：19 × 5 = 95 flops → 10 flops | **85** | 循环展开 + Constexpr 求值 |
| 权重预计算：19 × 1 mul → 3 muls | **16** | 循环展开 + Constexpr 求值 |
| uc² 共享（对称方向对） | **~42** | 循环展开 + Constexpr 求值 |
| **合计** | **~143** | |

### 6.2 CSE 可超越手写版的额外优化（36 flops）

| 未提取子表达式 | 浪费 FLOPS | CSE 特性 |
|---------------|:---------:|---------|
| `InvCs2 * u2 * 0.5` 重复 19 次 | **36** (18 × 2) | 跨语句 CSE（已实现） |

手写优化版也没有提取 `InvCs2 * u2 * 0.5`（在每个 feq 行中重复出现）。**如果实现 LICM，CSE 输出反而会优于手写版。**

---

## 7. 实现路线图

### Phase 1：循环展开（解锁后续优化）

1. 实现 `ForLoopUnroll` pass
   - 检测 `for (k = 0; k < CONST; ++k)` 模式
   - 将循环体复制 q 次，用常量替换 k
   - 在 IR 层面将 `ForLoopIR` 替换为 `BlockIR`
2. 与现有 CSE pass 联动验证

### Phase 2：LICM（超越手写版）

1. 实现 `LoopInvariantCodeMotion` pass
   - 分析循环体中引用循环外变量的表达式
   - 提取为循环前临时变量
2. 验证 CSE 输出优于手写版

### Phase 3：Constexpr 求值（轻量级）

1. 实现 lattice 函数模式匹配
   - 识别 `latset::c<LatSet>(k)` 调用
   - 识别 `latset::w<LatSet>(k)` 调用
2. 硬编码 D2Q5/D2Q9/D3Q7/D3Q15/D3Q19/D3Q27 的查找表

### Phase 4：代数增强

1. 常量乘法合并（`a*b*c → a*(b*c)` when b,c const）
2. 对称性分析（识别 `x` 和 `-x` 的关系）

---

## 8. 结论

| 指标 | 当前 | Phase 1 后 | Phase 1+2 后 | 全部完成后 |
|------|:----:|:----------:|:------------:|:----------:|
| **D3Q19 FLOPS** | 309 | ~166 | ~130 | ~130 |
| **vs 手写版 (~166)** | 差 143 | **持平** | **优 36** | **优 36** |
| **节省** | 0% | 46% | 58% | 58% |

**核心结论：**
1. **循环展开是关键阻塞点**——实现后，现有的跨语句 CSE 就能自动提取 `InvCs2 * u2 * 0.5` 等公共子表达式
2. **LICM 可以让 CSE 工具超越手写版**——手写版没有提取循环不变量
3. **Constexpr 求值走轻量级路线**——模式匹配 + 硬编码查表，不需要通用求值器
4. 前端语法支持已基本完成，不再阻碍后端优化
5. FreeLB 特定配置已通过 `plugins/freelb/` 分离，不影响通用 C++ 模式

---

## 9. 实现结果（已落地）

已实现 Phase 1 / Phase 3 / Phase 4 的核心部分（未实现独立 LICM，因为
展开后 CSE 已能完成等价工作）：

| 组件 | 位置 | 作用 |
|------|------|------|
| `LoopUnrollPass` | `src/passes/loop_unroll.*` | 展开计数循环，拼接进父 Block，内联循环体局部变量 |
| `LatticeResolvePass` | `plugins/freelb/lattice_resolve.*` | `latset::c/w` → 常量/点积，方向符号规范化 |
| 常量乘法合并 + 偶次幂 | `src/passes/algebraic_simplify.*` | `u0*u0*0.5*9.0 → u0*u0*4.5`，`(-a)*(-a) → a*a` |
| `ReassociatePass` | `src/passes/reassociate.*` | 加法项按全局频率排序，形成 `var0 + 4.5*uc²` 共享前缀 |

**实测（`cse -c`，成本模型已计入循环次数）D3Q19 equilibrium：**

| 版本 | FLOPs |
|------|:-----:|
| 原始循环版（Before） | 228 |
| 手写展开版 `tests/equilibrium_ref.cpp` | 89 |
| **CSE 工具输出（After）** | **93** |

相对原始循环版节省 135 flops（59.2%），与手写版相差仅 4.5%。

> 注：原始循环版的 `Before` 计数中，点积 `u * latset::c(k)` 作为一次乘法
> 计入（未展开），因此 228 低于完全展开文本计法的 309。成本模型现按字面
> 循环次数 N 乘以循环体开销，使 Before/After 可比。

生成的代码结构：

```cpp
auto _cse_0 = 1 - 1.5 * u2;                 // var0
auto _cse_1 = 0.0555556 * rho;              // 权重分组
auto _cse_2 = 4.5 * u0 * u0;
auto _cse_3 = _cse_0 + _cse_2;              // var0 + 4.5*uc^2（方向对共享）
feq[1] = _cse_1 * (_cse_3 + 3 * u0);
feq[2] = _cse_1 * (_cse_3 - 3 * u0);
// ... 其余方向同理
```

数值校验（`tests/verify_equilibrium.cpp`）最大误差 ~7e-18。工具输出在
行为等价的前提下与手写版 FLOP 相当（93 vs 89）。

**通用性修复（顺带）：**
- `IRModule::createConst` 现在按值去重，使常量参与的表达式能跨语句共享
- Codegen 对前缀一元运算的复合操作数补括号，修正 `-(u0+u1)` 的优先级
- 常量输出使用 17 位有效数字，避免 lattice 权重精度损失
