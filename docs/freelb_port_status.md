# FreeLB 移植状态（分支 `freelb-port`）

本文档汇总 `cse` 仓库 `freelb-port` 分支相对 `main` 已完成的工作与待办事项，
以及配套的 FreeLB 侧集成（`~/FreeLB` 分支 `dev-cse2`）。

## 1. 目标与架构

把通用 DAG-CSE 引擎用于生成 FreeLB 的“展开循环”特化头（`.ur.h`），替换
FreeLB `dev-cse` 分支中 `tools/cse` 的旧解释器/优化器。

分层：

- **引擎（本仓库）**：通用前端/IR/passes/CodeGen + FreeLB 专用插件
  （`plugins/freelb/`），产出 `libcse.a/.so` 与两个可执行文件：
  - `bin/cse`：通用 CSE CLI（`//@cse` 函数区域）。
  - `bin/csegen`：FreeLB `.ur.h` 生成器（`csegen <input.h> <output.h>`）。
- **驱动（FreeLB 仓库）**：`third_party/cse` submodule + `tools/cse/` 薄接线，
  `make.mk` 在 `-D_UNROLLFOR` 时调用 `csegen` 到 `generated/` 影子包含目录。

当前分支/提交：

| 仓库 | 分支 | 关键提交 |
|------|------|----------|
| `cse` | `freelb-port` | `edd01e8`(P0) → `6dd89a1`(P1) → `f9921d9`+`9dece9a`(P2) |
| `FreeLB` | `dev-cse2` | `f1683fe`(P0) → `2b1fa8d`(P1) → `28a0023`+`a2b7501`(P2) |
| FreeLB submodule | `third_party/cse` | 指向 `9dece9a` |

## 2. 已完成

### P0 — equilibrium（提交 `edd01e8`）
- `csegen <in.h> <out.h>`：按 `// @cse` 标记解析模板结构体，逐 latset 实例化，
  运行引擎管线，发射 `.ur.h`（`#pragma once` / include / `#ifdef _UNROLLFOR` /
  namespace / `CELL<T,LatSet<T>,TypePack>` 偏特化 / 6 个 latset）。
- 前端保真：保留 `static`/`inline`/`const` 与引用 `&`（lexer 增 `Amp`，
  `parseType` 修复前缀与引用）。
- `CSEConfig` per-latset 上下文（`latsetAlias/latsetName/latsetDim/latsetQ/latsetCs2`）：
  `<LatSet>::q/d/cs2/InvCs2/InvCs4` 折叠为常量。
- `plugins/freelb/lattice_resolve`：全部 6 个 latset、按别名解析
  `latset::c<LatSet>/w<LatSet>`、权重符号规范化为 `latset::w<LatSet>(k)`。
- `CodeGen::generateBody`：仅发射方法体。
- 验证：`verify_equilibrium.py` 6/6（D2Q5/D2Q9/D3Q7/D3Q15/D3Q19/D3Q27）。

### P1 — force（提交 `6dd89a1`）
- 向量降级（`CSEConfig.lowerVectors`）：`Vector<T,LatSet::d>` 的分量式
  `+ - * /`、点积、常量分量索引、`latset::c<LatSet>(k)` 方向向量。
- 非类型模板参数：`CSEConfig.constBindings`（如 `ScalarForcePopImpl` 的 `d`）。
- 驱动按模板形参分类：`<CELL>` / `<T,LatSet>` / `<T,LatSet,非类型>`，
  非类型形态按 `d=0..dim-1` 逐个实例化。
- 修复符号常量折叠：`ConstantFold` / `AlgebraicSimplify::normalizeProduct`
  不再把 `latset::w<LatSet>(k)` 的数值折掉、丢失符号。
- 验证：`verify_force.py`（`ForcePopImpl` + `ScalarForcePopImpl`）6/6。

### P2 — moment（提交 `f9921d9`、`9dece9a`）
- 前端：`T{}` 值初始化、`x.template f<...>()`、`if constexpr`、
  模板/限定类型局部声明（`looksLikeVarDecl` 前瞻）、声明处 `{}` 初始化。
- 向量：向量局部按分量降级并登记；向量赋值 / 元素存储 / 整向量存储；
  记录可索引基节点，使 `latset::c<LatSet>(k)[alpha]` 保留到 `alpha` 变常量。
- 循环展开：支持非零起点（三角循环 `for (beta=alpha;...)`）；对可变/不纯局部
  按迭代重命名（`name__u<i>`）；`freshen` 让每迭代获得独立的不纯 load；
  递归展开由代入而暴露的内层循环；用当前子树重新计算使用数（避免陈旧计数）。
- 常量/语句：比较运算常量折叠（`== != < > <= >=`）；`substitute` 保留
  `++`/`--` 并重建 `Cast`/`Ternary`；`++`/`--` 标记为不纯。
- 新增 `CounterPropPass`：直线计数器解析（`tensor[i]→tensor[0],…`）、
  降级向量局部索引（`unew[1]→unew_1`）、常量条件折叠（`if (alpha==beta)`）。
- `if constexpr` 单语句无花括号发射（贴合 FreeLB 风格/验证脚本）。
- 验证：`verify_moment.py` **60/60**（11 结构体 × 6 latset）。

### FreeLB 侧（`dev-cse2`）
- `third_party/cse` submodule；`tools/cse/` 旧解释器（约 2886 行）删除，
  `Makefile` 改为“构建引擎 → 复制 `csegen` → `gen/verify/install`”。
- `make.mk`：`-D_UNROLLFOR` 时 `UR_CSE_BASES ?= lbm/moment lbm/equilibrium lbm/force`
  生成到 `generated/` 并用 `-Igenerated` 影子包含；未列出的手写 `.ur.h` 回退。
- `tools/cse/verify_{moment,equilibrium,force}.py` 保留并接入 `make verify`。

### 验证矩阵（当前）
| 项 | 结果 |
|----|------|
| `verify_moment.py` | 60/60 |
| `verify_force.py` | ALL PASSED |
| `verify_equilibrium.py` | PASS |
| 引擎回归 `tests/{test1,test_all,equilibrium_d3q19,safety_cases}` | 10 / 50 / 84 flops / PASS |
| `examples/cavity3d -D_UNROLLFOR` | 编译通过（0 error） |

## 3. 已实现的接口/配置（供扩展参考）

- CLI：`csegen <input.h> <output.h>`；`input` basename 决定 include/namespace
  （仅 `moment.h`/`equilibrium.h`/`force.h`）。
- `CSEConfig`（`src/frontend/cse_config.h`）：
  - `assumeNumericCommutative/Associative`、`allowFpReassoc`、`noAlias`、`isPureFunction`
  - `latsetAlias/latsetName/latsetDim/latsetQ/latsetCs2`
  - `lowerVectors`、`constBindings`
- `plugins/freelb/ur_emit.{h,cpp}`：`generateUrHeader(in,out)`、`detectUrConfig`。
- `plugins/freelb/lattice_resolve.{h,cpp}`：`createLatticeResolvePass(config)`。
- 结构体分类（`ur_emit.cpp`）：`Cell` / `CellType` / `TLatSet` / `TLatSetD`。

## 4. 未完成 / TODO

### 高优先级（发布前）
1. ~~推送与依赖 URL~~ **已完成**：`freelb-port` 已推到
   `/mnt/d/gitservice/cse.git`；FreeLB `.gitmodules` 使用
   `/mnt/d/gitservice/cse.git`。注意该远端是本地 bare 仓库，克隆
   FreeLB 时需允许 file 协议：
   `git -c protocol.file.allow=always submodule update --init`
   （或 `git config protocol.file.allow always`）。若后续发布到 GitHub，
   用一条命令改 URL 即可（`git config -f .gitmodules submodule.third_party/cse.url <url>`）。
2. **统一测试入口**：引擎目前无 `make test`/CI；建议加一个脚本运行
   `cse` 回归 + `csegen tests/ur/*.h` + 三个 Python 验证（需要 FreeLB 参考头）。
3. **发布构建/安装**：引擎默认 `-O0`；建议提供 `-O2` release、可选
   `install`（头 + 库 + pkg-config）与版本/SONAME，便于作为库复用（方案 2）。

### 中优先级
4. **latset 表同步**：`lattice_resolve.cpp` 的速度向量/权重是手抄自
   `src/lbm/lattice_set.h`，存在漂移风险；应改为从 FreeLB 头生成或加校验。
   目前仅 6 个 latset（无 D1Q3/D2Q4）。
5. **`make install-ur` 语义**：`verify` 比较的是 `src/lbm/*.ur.h`（当前
   `moment.ur.h` 已是生成版），安装后再次验证会退化为“自己比自己”。需要
   保留手写参考副本或调整流程（verify 用上游参考，再 install）。
6. **文档**：`tools/cse/DESIGN.md` 的“架构总览”等章节仍是旧解释器描述，
   应整体重写为“引擎 + 驱动”的新结构。
7. **CI**：FreeLB 侧在 `-D_UNROLLFOR` 下至少编译一个示例；引擎侧跑回归。

### 低优先级 / 已知限制
8. **前端子集限制**：
   - `if constexpr` 条件保持不透明（不折叠）；只有普通 `if` 的常量条件会折叠。
   - 循环展开仅支持常量上下界（`for (k=0;k<N;…)`，含非零起点）；命名/表达式
     上界不展开。
   - `Vector` 语义仅覆盖点积与分量运算，无矩阵/张量运算、无通用构造。
   - `opp`、`hasField`、`getOmega`、`getnorm2` 等当作不透明调用（不去重/不外提）。
   - 向量元素符号索引（如 `u_value[ForceScheme::scalardir]`）仅按原文发射。
9. **`CounterProp` 的向量局部映射**依赖命名约定（`unew[1]→unew_1`），
   属启发式；若后续引入更复杂作用域需改为显式映射。
10. **submodule 工作流摩擦**：引擎每次改动需“引擎提交 + FreeLB 指针 bump”两步。
11. **多消费者支持**：目前只有 FreeLB 一个消费者；若需复用，需走安装/导出路线。

## 5. 如何构建与验证

引擎：
```bash
cd /home/ym/code/cse
make                       # 生成 bin/cse, bin/csegen, libcse.a/.so
./bin/csegen tests/ur/moment.h /tmp/moment.ur.h
```

FreeLB（`dev-cse2`）：
```bash
cd ~/FreeLB/tools/cse
make                       # 构建引擎并复制 csegen
make gen                   # 生成 out/{moment,equilibrium,force}.ur.h
make verify                # 三个数值验证
# 示例（使用 generated/ 影子包含）
cd ~/FreeLB/examples/cavity3d && make
```

## 6. 相关文件

引擎（`freelb-port`）：
- `plugins/freelb/ur_emit.{h,cpp}`、`plugins/freelb/ur_emit_main.cpp`
- `plugins/freelb/lattice_resolve.{h,cpp}`、`plugins/freelb/config.h`
- `src/frontend/{cse_config.h,lexer.cpp,parser.cpp,parser.h,ast.h,token.h}`
- `src/ir/{ir_builder.h,ir_builder.cpp,ir_utils.h,statement.h}`
- `src/backend/codegen.{h,cpp}`
- `src/passes/{loop_unroll.cpp,counter_prop.h,counter_prop.cpp,pass_manager.cpp,value_prop.cpp,dce.cpp,algebraic_simplify.cpp}`
- `tests/ur/{moment.h,force.h,equilibrium.h}`

FreeLB（`dev-cse2`）：
- `third_party/cse`（submodule）、`.gitmodules`
- `tools/cse/{Makefile,DESIGN.md,verify_*.py}`
- `make.mk`、根 `Makefile`
