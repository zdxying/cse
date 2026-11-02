# FreeLB 移植状态

本文档是 FreeLB 移植状态的唯一权威副本；FreeLB 侧没有对应的状态文件。

## 1. FreeLB 如何消费引擎

`cse` 分为通用核心 `src/` 与 FreeLB 插件 `plugins/freelb/`，产出
`libcse.a/.so`、`bin/cse`（通用 `//@cse` CLI）和 `bin/csegen`（`.ur.h` 生成器）。

FreeLB 的接入：

- `third_party/cse` 是 submodule，`.gitmodules` 跟踪 `branch = main`。
- `tools/cse/` 只保留接线与验证：`make` 构建引擎并复制 `csegen`，`make gen`
  生成 `generated/lbm/{moment,equilibrium,force}.ur.h`，`make verify` 跑三个
  数值校验，`make cost` 用 `csegen --cost` 测成本。`tools/cse/reference/` 是
  冻结的手写 `.ur.h` 对比基准，从不上包含路径。
- `make.mk` 在 `-D_UNROLLFOR` 且 `csegen` 存在时加
  `-D_FLB_UR_CSE -I$(GEN_DIR)`，并按需从 `src/lbm/*.h` 生成 `.ur.h`；`csegen`
  缺失时告警并回退到 `src/lbm` 的循环版（`make.mk:85-101`）。
- FreeLB CI（`.github/workflows/ci.yml`）两个 job：`engine` 构建并跑 pinned
  submodule 的 `make test`；`unrollfor` 构建 csegen、跑
  `make -C tools/cse verify`，断言 dry-run 带
  `-D_FLB_UR_CSE -I../../generated`，并在有/无 xcore 下各编译一次
  `examples/cavity3d`。断言是必需的一环：回退路径让“构建成功”本身不证明
  生成头被使用。

## 2. 验证状态

| 项 | 当前结果 / 入口 |
|----|----------------|
| FreeLB `verify_moment.py` | 60/60（对 `generated/lbm/moment.ur.h` 逐项数值比对） |
| FreeLB `verify_force.py` | ALL PASSED（12 个特化） |
| FreeLB `verify_equilibrium.py` | PASS |
| 引擎回归 | `make test`：FLOP 钉（default / `-r` / `-s`）、数值校验器、扫描 oracle、`csegen` 冒烟；夹具与钉值清单见 `tests/README.md` |
| 引擎 sanitizer | `make sanitize`（ASan/UBSan） |
| `csegen` 冒烟 | `tests/csegen/*.h` 生成并检查代表特化；每次运行向 stderr 报告覆盖率；生成头断言无 `a[i] = a[i];` |

`make test` 在存在 FreeLB checkout（`FREELB=` 或 `~/FreeLB`）时追加
`check_lattice.py` 与三个 `verify_*.py`，否则跳过。

## 3. 接口与关键配置

`CSEConfig` 字段、通用扩展点、默认管线插槽与 `csegen` 的输入约定见
`docs/architecture.md`「配置系统」「插件注入」。FreeLB 侧只需记住：

- `csegen [--cost] [--json] [--lattice NAME]... <input.h> <output.h>`：输入
  basename 必须是 `moment.h` / `equilibrium.h` / `force.h`；`--lattice` 可重复；
  每次运行向 stderr 打印区域/结构体/发射/跳过覆盖率。
- `LatticeConfig`（`plugins/freelb/config.h`）承载 `alias/name/dim/q/cs2`；
  `createFreeLBConfig(latCfg)` 把它注入 `resolveName`、向量降级与成本钩子。
  `CSEConfig` 本身没有 latset 字段。
- **`noAlias`**：`const T*` / `const T&` 形参都可能与另一个实参指向同一对象
  （`f(x, x)`），所以通用默认 `false`，引用形参的 load 不跨写共享。FreeLB
  kernel 的 cell / momenta / force 缓冲互不别名，profile 显式置 `true`。
  实测 `csegen --cost` 的 `force.h`：`noAlias=true` 时 D3Q19/D2Q9 的 After 为
  353/150；临时关闭该许可（同一管线）退到 671/224。`bin/cse -s` 保持关闭。

## 4. 未完成 / 已知限制

- **前端子集**：
  - `if constexpr` 条件保持不透明（不折叠）；FreeLB 管线里的 `CounterPropPass`
    会折叠普通 `if` 的常量条件（`plugins/freelb/counter_prop.cpp`）。
  - 循环展开只支持常量起点与常量上界（`for (k=a; k<N; ++k)`，最多 64 次）；
    上界为命名或表达式时不展开（`src/passes/loop_unroll.cpp`）。
  - `Vector` 只覆盖分量式 `+ - * /`、点积、常量分量索引和 `latset::c` 触发；
    无矩阵/张量运算，无通用构造。
  - `opp` 等未登记的调用按不透明/不纯处理；`getOmega`、`template get<>` /
    `hasField<>`、`getnorm2` / `getsum` / `getnorm` / `dot` 已由
    `isFreeLBPureFunction` 登记为纯函数，可去重和提取。
  - 符号索引（如 `u_value[ForceScheme::scalardir]`）按原文发射；索引变成常量
    后才解析。
- **`CounterProp` 的向量局部映射**依赖 `vectorLocalName` 的命名约定
  （`unew[1] → unew_1`），是启发式；作用域变复杂时需改为显式映射。
- **submodule 工作流**：引擎每次改动需要“引擎提交 + FreeLB 指针 bump”两步。
- **单消费者**：目前只有 FreeLB 使用；未提供 pkg-config 或 SONAME。
- **latset 覆盖**：`.ur.h` 仅覆盖 D2Q5/D2Q9/D3Q7/D3Q15/D3Q19/D3Q27；
  D1Q3/D2Q4 不在生成覆盖内。
- **`verify_force.py` 的语义**：忽略 reference 参数，只读生成文件并按内置公式
  校验；另外两个脚本对新旧文件逐项数值比较。
- **collision / MRT 不在覆盖内**：`tools/cse/Makefile` 记录原因（C 数组声明、
  `unsigned int`、stride-2 bounce-back）；手写展开版本仍作为普通头文件使用。

## 5. 构建与验证

引擎（本仓库根）：

```bash
make                       # 生成 bin/cse, bin/csegen, libcse.a/.so
make test                  # FLOP 回归 + 数值校验 + csegen 冒烟（含 FreeLB 段）
./bin/csegen tests/csegen/moment.h /tmp/moment.ur.h
```

FreeLB：

```bash
cd ~/FreeLB/tools/cse
make                       # 构建引擎并复制 csegen
make gen                   # 生成 generated/lbm/*.ur.h
make verify                # 三个数值验证
make cost                  # csegen --cost，默认 LATTICES="D2Q9 D3Q19"
# 示例（使用 generated/ 影子包含）
cd ~/FreeLB/examples/cavity3d && make
```

## 6. 相关文件

- 引擎目录结构、`CSEConfig` 字段、pass 管线、成本模型：`docs/architecture.md`。
- 引擎入口与测试：`Makefile`、`tests/run_tests.sh`、`tests/README.md`。
- FreeLB 侧：`third_party/cse`（submodule）、`tools/cse/{Makefile,reference/,verify_*.py}`、
  `make.mk`、`.github/workflows/ci.yml`。

## 7. 通用 / FreeLB 边界

`src/` 是零 FreeLB 语义的通用引擎：不 include `plugins/`，`CSEConfig` 没有
latset 字段。判定标准是“无语义依赖”而非“零字面出现”——
`grep -riE "freelb|latset|lattice" src/` 只命中 6 行注释，它们举例说明通用规则
（符号常量不进身份、零方向分量、引用别名），不参与任何判断。

FreeLB 的行为只经通用扩展点进入：`CSEConfig::{resolveName, lowerVectors,
vectorDim, isVectorType, isVectorProducingCall, vectorLocalName, constBindings,
noAlias, assumeFloatOperands}` 与 `PassManager::createDefault(..., resolvePass,
postAlgebraPass)`。签名与语义见 `docs/architecture.md`「配置系统」；FreeLB 的
实现全部在 `plugins/freelb/`（`config.h`、`lattice_resolve`、`counter_prop`、
`ur_emit`、`cuda_skip` 与两个入口）。

`tests/csegen/*.h` 与 `tests/verify/check_lattice.py` 是 FreeLB 专属测试。
