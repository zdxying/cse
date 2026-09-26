# FreeLB 移植状态

本文档汇总 `cse` 引擎为 FreeLB 所做的移植工作与待办事项，以及配套的 FreeLB 侧
集成（`~/FreeLB` 分支 `main`）。

> **唯一权威副本**：本文档是移植状态的单一来源。FreeLB 侧的
> `tools/cse/PORT_STATUS.md` 只是指回本文件的占位指针，不要在那边编辑。

> **分支状态**：原 `freelb-port` 工作已 fast-forward 合入 `main`
> （`5296c74..3ef4147`），`main` 现在即"通用引擎 + FreeLB 插件 + `.ur.h` 生成器"。
> 后续 FreeLB 集成提交直接走短生命周期分支合入 `main`。本文档中出现的
> `freelb-port` 为历史提交所在分支，提交号在 `main` 上同样可达。

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

当前分支/提交（HEAD 列为本文档更新时 2026-09-25 的快照）：

| 仓库 | 分支 | 关键提交 | 快照 HEAD |
|------|------|----------|-----------|
| `cse` | `main`（原 `freelb-port`） | `edd01e8`(P0) → `6dd89a1`(P1) → `f9921d9`+`9dece9a`(P2) | `8b8c47b` |
| `FreeLB` | `main` | `f1683fe`(P0) → `2b1fa8d`(P1) → `28a0023`+`a2b7501`(P2) | `782e3df` |
| FreeLB submodule | `third_party/cse` | 跟踪 `branch = main` | 指针随每次引擎提交同步 bump |

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
- 验证：`verify_moment.py` **60/60**（`reference` 共 11 结构体 × 6 latset = 66 个
  特化，脚本解析正则当前覆盖其中 60 个，逐项数值比对全通过）。

### FreeLB 侧（`main`）
- `third_party/cse` submodule；`tools/cse/` 旧解释器（约 2886 行）删除，
  `Makefile` 改为“构建引擎 → 复制 `csegen` → `gen/verify/install`”。
- `make.mk`：`-D_UNROLLFOR` 时 `UR_CSE_BASES ?= lbm/moment lbm/equilibrium lbm/force`
  生成到 `generated/` 并用 `-Igenerated` 影子包含；未列出的手写 `.ur.h` 回退。
- `tools/cse/verify_{moment,equilibrium,force}.py` 保留并接入 `make verify`。

### 验证矩阵（当前）
| 项 | 结果 |
|----|------|
| `verify_moment.py` | 60/60 |
| `verify_force.py` | ALL PASSED（对生成文件按解析公式校验） |
| `verify_equilibrium.py` | PASS |
| 引擎 FLOP 回归 `tests/fixtures/*` | `basic_cse` 10 / `features` 50 / `namespace_case` 4 / `equilibrium_d3q19` 84 / `safety_cases` 20 flops |
| 引擎数值校验 `tests/verify/{verify_equilibrium,verify_safety}.cpp` | 误差阈值内 / ALL SAFETY CHECKS PASSED |
| `csegen tests/csegen/*.h` 冒烟 | equilibrium / force / moment 各检出代表特化 |
| `examples/cavity3d -D_UNROLLFOR` | 编译通过（0 error） |

以上由引擎 `make test`（`tests/run_tests.sh`）一次执行；FreeLB 段仅在存在
FreeLB checkout（`FREELB=` 或 `~/FreeLB`）时运行。

## 3. 已实现的接口/配置（供扩展参考）

- CLI：`csegen <input.h> <output.h>`；`input` basename 决定 include/namespace
  （仅 `moment.h`/`equilibrium.h`/`force.h`）。
- `CSEConfig`（`src/frontend/cse_config.h`，通用，无 FreeLB 语义）：
  - `assumeNumericCommutative/Associative`、`allowFpReassoc`、`noAlias`、`isPureFunction`
  - 钩子：`resolveName`、`lowerVectors` + `vectorDim/isVectorType/isVectorProducingCall`、
    `vectorLocalName`、`constBindings`
- `plugins/freelb/config.h`：`LatticeConfig`、`resolveLatsetConst`、
  `createFreeLBConfig(latCfg)`，把 latset 上下文经上述钩子注入。
- `PassManager::createDefault(config, recombine, resolvePass, postAlgebraPass)`：
  插件在此注入 `LatticeResolvePass` 与 `CounterPropPass`。
- `plugins/freelb/ur_emit.{h,cpp}`：`generateUrHeader(in,out)`、`detectUrConfig`。
- `plugins/freelb/lattice_resolve.{h,cpp}`：`createLatticeResolvePass(config)`。
- 结构体分类（`ur_emit.cpp`）：`Cell` / `CellType` / `TLatSet` / `TLatSetD`。

## 4. 未完成 / TODO

### 高优先级（发布前）
1. ~~推送与依赖 URL~~ **已完成**：`main` 已推到两个远端——`origin`
   （`git@github.com:zdxying/cse.git`）与 `local`（`/mnt/d/gitservice/cse.git`，
   本地 bare）；FreeLB `.gitmodules` 使用 GitHub URL 并跟踪 `branch = main`。
   后续再换 URL 用一条命令即可
   （`git config -f .gitmodules submodule.third_party/cse.url <url>`）。
   注：早期"克隆 FreeLB 需 `protocol.file.allow=always`"的说明针对旧的本地
   file 远端，现用 SSH URL 已不再需要。
2. ~~统一测试入口~~ **已完成**：`make test`（`tests/run_tests.sh`）执行
   FLOP 代价回归 + `verify_equilibrium`/`verify_safety` 数值校验 +
   `csegen tests/csegen/*.h` 冒烟；若存在 FreeLB checkout（`FREELB=` 或
   `~/FreeLB`）再跑 `tools/cse/verify_{moment,equilibrium,force}.py`。
   CI 接入仍可后续补。
   - 顺带修复了 `cse_pass` 的**候选选择不确定性**（仅按语句数排序，平局时保留
     `unordered_map`（指针键）遍历序），使输出依赖堆布局/输入路径；现按
     `node->id` 断平局，输出确定且与路径无关。
3. ~~发布构建/安装~~ **已完成**：`make release`（`OPT=-O2` 重新构建）、
   `make install PREFIX=... DESTDIR=...` 安装 `cse`/`csegen` 与
   `libcse.a/.so`。头文件仍以 submodule + 源码编译方式消费，未提供
   pkg-config/SONAME。

### 中优先级
4. ~~latset 表同步~~ **已完成（防漂移）**：`tests/verify/check_lattice.py` 解析
   FreeLB `latsetdata::c<D,Q>`/`w<D,Q>` 与引擎 `kDxQy{c,w}` 并逐一比对，
   夹具与权重不一致即失败；`make test` 在存在 FreeLB checkout 时自动运行。
   仍仅覆盖 6 个 latset（无 D1Q3/D2Q4，两者未被 `.ur.h` 使用）。
5. ~~`make install-ur` 语义~~ **已完成**：手写参考快照固定到
   `tools/cse/reference/{moment,equilibrium,force}.ur.h`，`verify` 改为与
   reference 比较，`install` 覆盖 `src/lbm/*.ur.h` 后再次 verify 仍有意义
   （reference 变化时用 `make gen-refs` 重新快照）。
   注：`verify_moment.py`/`verify_equilibrium.py` 真的对比 reference 与生成文件；
   `verify_force.py` 只读入生成文件、按内置解析公式校验（传入的 reference 参数被忽略）。
6. ~~文档~~ **已完成**：`tools/cse/DESIGN.md` 已重写为“引擎 + 驱动”结构。
7. **CI**：FreeLB 侧在 `-D_UNROLLFOR` 下至少编译一个示例；引擎侧跑回归。
   （用户侧 CI 未接，`make test` 已可手动/脚本调用。）

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
cd third_party/cse
make                       # 生成 bin/cse, bin/csegen, libcse.a/.so
make test                  # FLOP 回归 + 数值校验 + csegen 冒烟（含 FreeLB 段）
./bin/csegen tests/csegen/moment.h /tmp/moment.ur.h
```

FreeLB（`main`）：
```bash
cd ~/FreeLB/tools/cse
make                       # 构建引擎并复制 csegen
make gen                   # 生成 out/{moment,equilibrium,force}.ur.h
make verify                # 三个数值验证
# 示例（使用 generated/ 影子包含）
cd ~/FreeLB/examples/cavity3d && make
```

## 6. 相关文件

引擎（`main`）——`src/` 为通用核心，`plugins/freelb/` 为 FreeLB 集成：
- `src/frontend/{cse_config.h,lexer.cpp,parser.cpp,parser.h,ast.h,token.h}`
- `src/ir/{ir_builder.h,ir_builder.cpp,ir_utils.h,statement.h}`
- `src/backend/codegen.{h,cpp}`
- `src/passes/{loop_unroll.cpp,counter_prop.h,counter_prop.cpp,pass_manager.cpp,value_prop.cpp,dce.cpp,algebraic_simplify.cpp}`
- `plugins/freelb/{config.h,lattice_resolve.{h,cpp}}`（配置钩子 + latset 解析）
- `plugins/freelb/{cse_main.cpp,ur_emit.{h,cpp},ur_emit_main.cpp}`（`bin/cse` 与 `bin/csegen` 驱动）
- `tests/csegen/{moment.h,force.h,equilibrium.h}`

FreeLB（`main`）：
- `third_party/cse`（submodule）、`.gitmodules`
- `tools/cse/{Makefile,DESIGN.md,PORT_STATUS.md,reference/,verify_*.py}`
  （`PORT_STATUS.md` 为指向 `third_party/cse/docs/freelb_port_status.md` 的占位指针）
- `make.mk`、根 `Makefile`

## 7. 通用 / FreeLB 边界（已完成剥离）

`src/` 现为**零 FreeLB 语义的通用引擎**：`grep -riE "freelb|latset|lattice" src/`
为空，`src/` 不再 include `plugins/`，`CSEConfig` 无 latset 字段。FreeLB 的全部
行为经 `CSEConfig` 的通用钩子注入，由 `plugins/freelb/` 提供。

- **A. 通用引擎能力**（与 FreeLB 无关）：
  `src/frontend/{lexer,token,parser,ast}`（引用限定符、`if constexpr`、`T{}`、
  `T x{...}`、限定/模板类型声明前瞻、`.template f<...>()`）；
  `src/ir/{statement,ir_utils}`（`AssignIR::targetExpr`、`IfElseIR::isConstexpr`、
  `substitute` 重建 `Cast/Ternary` 与保留 `++/--`、比较折叠与符号常量保护）；
  `src/backend/codegen`（`generateBody`、`if constexpr` 发射、`targetExpr` 赋值）；
  `src/passes/{loop_unroll,counter_prop,dce,value_prop,algebraic_simplify,cse_pass}`；
  `Makefile` 的 `OPT/release/install/test` 与 `tests/run_tests.sh`。
- **B. 通用扩展点**（FreeLB 经此注入，不再内建 FreeLB 语义）：
  `CSEConfig::resolveName`（名字→常量，FreeLB 用于 `LatSet::q/d/cs2/...`）；
  `CSEConfig::{lowerVectors,vectorDim,isVectorType,isVectorProducingCall}`
  （通用向量降级算法，FreeLB 提供 `Vector`/`latset::c` 触发器）；
  `CSEConfig::vectorLocalName`（降级向量局部命名，供 `CounterPropPass`）；
  `PassManager::createDefault(..., resolvePass, postAlgebraPass)`（插件注入 pass）。
- **C. FreeLB 专属**（全部在 `plugins/freelb/`）：
  `config.h`（`LatticeConfig`、`resolveLatsetConst`、`createFreeLBConfig`）、
  `ur_emit.{h,cpp}`、`ur_emit_main.cpp`、`cse_main.cpp`、
  `lattice_resolve.{h,cpp}`、`cuda_skip.{h,cpp}`；
  以及 `tests/csegen/*.h`、`tests/verify/check_lattice.py`、`Makefile` 的
  `bin/csegen` 文件目标。
