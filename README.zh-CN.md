# CSE — 一个小型 C++ 公共子表达式消除优化器

[English](README.md) | [中文](README.zh-CN.md)

CSE 分析你用 `//@cse` 标记的函数，找出跨语句重复的计算，并把去重后的区域写回。
它是独立的 C++17 工具，采用受 LLVM 启发的三阶段流水线 **前端 → IR → Pass → 后端**，
以「库 + 两个可执行文件」的形式交付。

```cpp
//@cse
double compute(double a, double b, double c, double d, double e) {
    double t1 = a * b * c * d * e;
    double t2 = b * c * d;
    double t3 = c * d;
    double t4 = a * b;
    return t1 + t2 + t3 + t4;
}
```

## 特性

- **基于 DAG IR 的跨语句 CSE**：IR 构建阶段对表达式做哈希去重，再由专门的 pass
  提取分散在不同赋值语句中的公共子表达式。
- **计数循环展开**：`for (i = 0; i < N; ++i)` 展开为逐条语句，这是对循环体做任何
  跨语句优化的前提。
- **安全默认、按需放开**：`CSEConfig` 默认保守——不假设交换律、不做重结合、
  不假设无别名、未知调用一律视为有副作用；`cse` 命令行以 FreeLB 配置（代数规则
  开启）启动，加 `-s` 即切回保守语义。
- **项目逻辑走钩子，不进核心**：`src/` 不含任何 FreeLB 知识，格子相关的全部行为
  通过 `CSEConfig` 钩子注入到 `plugins/freelb/`。
- **可度量的输出**：`-c` 报告优化前后的 FLOP 数，`--json` 以机器可读格式输出，
  `make test` 把这些数字固定为回归表。

## 流水线

```
LoopUnroll → [Resolve] → ConstantFold → AlgebraicSimplify → [PostAlgebra]
  → [Reassociate] → CSEPass → [ExprRecombine → AlgebraicSimplify] → ValueProp → DCE → Cleanup
```

方括号中的环节可选或受配置门控。各 pass 的说明、插件槽位与完整的安全模型见
[docs/architecture.md](docs/architecture.md)。

## 环境要求

- GNU make
- 支持 C++17 的编译器（默认 `g++`，可用 `CXX=...` 覆盖）
- Python 3（仅 `make test` 内的 latset 检查需要）

## 构建

```bash
make            # 生成 bin/cse、bin/csegen、bin/libcse.a、bin/libcse.so
make test       # 构建并运行完整回归
make release    # 清理后以 -O2 重新构建
make install PREFIX=/usr/local DESTDIR=
make clean
```

## 用法

### 分析并优化 `//@cse` 区域

```bash
./bin/cse input.cpp -c          # 报告优化前后的 FLOP 代价
./bin/cse input.cpp             # 优化并写出 input.cpp.cse
./bin/cse input.cpp -r -v       # 启用表达式重组，并打印每个 pass
./bin/cse input.cpp -s -c --json
```

区域从**去除缩进后以 `//@cse` 开头的那一行**开始，到下一个配平的 `{ ... }` 函数体
结束。区域之外的内容原样保留，输出文件固定为源文件旁的 `<input>.cse`。

| 选项 | 作用 |
|------|------|
| `-c`, `--cost` | 打印优化前后的 FLOP / 向量操作 / 访存 / 节点 / 语句计数 |
| `--json` | 以 JSON 输出代价报告（配合 `-c`） |
| `-r`, `--recombine` | 启用提取公因子（`a*x + a*y → a*(x+y)`） |
| `-s`, `--safe` | 面向任意 C++ 的保守语义 |
| `-v`, `--verbose` | 逐 pass 打印到 stderr |
| `--dump-ir` | 打印每个 pass 前后的 IR（到 stdout） |
| `--verify` | 检查 pass 不变量（效果保持）；违规时退出码 4 |
| `--lattice NAME` | 代价模型使用的 lattice 集合（D2Q5、D2Q9、D3Q7、D3Q15、D3Q19、D3Q27） |
| `-h`, `--help` | 用法 |

### 生成 FreeLB 的 `.ur.h` 特化

```bash
./bin/csegen tests/csegen/moment.h /tmp/moment.ur.h
./bin/csegen --cost --lattice D3Q19 tests/csegen/moment.h /tmp/moment.ur.h
```

`csegen <input.h> <output.h>` 读取带 `// @cse` 标记的生产头文件，为每个受支持的格子集
（D2Q5、D2Q9、D3Q7、D3Q15、D3Q19、D3Q27）发射完全展开的特化。输入文件的 basename
必须是 `moment`、`equilibrium` 或 `force` 之一。

`--cost` 报告与生成同一管线测得的优化前后 FLOP（`--json` 机器可读，
`--lattice NAME` 可重复以限定格子集）。每次运行还会向 stderr 打印覆盖率报告
（区域数 / 结构体数 / 发射与跳过数 / 特化总数），生成器无法处理的结构体会显式
可见而不是被悄悄丢掉：
`csegen: coverage: 7 regions, 7 structs (7 emitted, 0 skipped), 42 specializations`。

FreeLB 通过 `third_party/cse` submodule 使用本工具：集成方式、构建入口与当前状态见
[docs/freelb_port_status.md](docs/freelb_port_status.md)。

## 安全模型

在保守模式下（`-s`，也是 `CSEConfig` 的默认值），优化通过八条机制保证行为等价：

1. **纯度**：调用默认视为有副作用，只有登记为纯的调用才允许去重或跨语句提取；
   任何"把两处出现折叠为一处"的重写都要求被折叠的子表达式是纯的。
   内建数学函数白名单只匹配**限定名**的自由调用（`std::sqrt`）：裸名 `max`/`min`/`abs`
   可能解析到用户自己的函数，合并两次调用会丢掉副作用；成员调用（`obj.sin`）同样不查白名单。
2. **内存**：只有只读根的 load 可共享；可变/未知根的 load 每次独立，避免跨 store 复用。
   **指针与引用形参（`T*`、`T&`、数组）一律不视为只读根**——`const` 只承诺"不通过它写"，
   不承诺没有别的名字指向同一对象（`f(x, x)` 可把 `const T&` 与 `T&` 绑到同一实参）；
   显式打开 `noAlias` 才恢复共享（FreeLB profile 已打开：其 kernel 的实参从不互为别名）。
3. **赋值可见性**：变量按名字驻留，重新赋值不产生新节点，同名子表达式仍是**同一个节点**。
   因此跨语句改写必须检查写操作：CSE 只在"定义点到每个使用点之间都没有写入其操作数"时
   才提取；值传播只内联"初值依赖的变量在整个函数内都没被写过"的定义。元素/成员写
   （`a[i] = v`、`p->f = v`）算作对 lvalue **根**的写——可能有别的指针指向同一对象，
   这是保守答案。
4. **支配**：位于 `if`/`else`/循环内的表达式标记为 nested，绝不外提。
5. **可投机性**：`isSpeculatable`（`analysis/purity.h`）回答的是另一个问题——
   *这个求值能不能放到源码不会执行它的位置？* 它与"能否重复求值"只在陷阱处分歧：
   `1/k` 可以重复求值（值相同）但不可投机（`k=0` 时 trap）。这是
   `k ? 1/k : 0` 的修复：该除法曾被提到条件之外，`k=0` 时直接崩溃。
   守卫操作数（三元的两臂、`&&`/`||` 的右操作数）只有在可投机时才提取；
   提取**整个**条件表达式始终安全（它保留内部短路），所以只需检查条件/左操作数。
6. **作用域**：遮蔽变量做 alpha-rename，同名但无关的声明不会被误合并。
   当 region **读取**一个它从未声明的名字时（`double s = t;` 而 `t` 是全局），
   后续的 `double t = a;` 也必须改名，否则两者会成为同一个节点。
   所有生成名（`__s`、`__u`、`_cse_`）都来自 `IRModule::freshName`，
   因此永远不会与用户标识符撞名。
7. **展开前提**：只有当循环体声明的每个局部变量都能被内联或安全改名、循环变量未被循环体内的
   `++`/`--` 改写、**循环变量不在循环语句之外被读**、且循环体不含属于本循环的
   `break`/`continue` 时才展开，否则保留循环。循环变量还须在 `for` 初始化子句中声明
   （初始化子句为赋值时也不展开）。
8. **递归有界**：表达式解析深度上限 512 层，超限报 `CSE-PARSE-DEPTH` 并把区域原样透传，
   而不是栈溢出崩溃。

代码生成还会**原样打印它拿到的树**：同优先级的右子节点一律保留括号——IEEE-754 下不仅
`a - (b - c)`、`a / (b * c)` 不等于去括号的形式，连 `a * (b * c)` 与 `(a * b) * c`
的舍入也不同。

代数规则按授权类型分开：交换律/结合律用 `assumeNumericCommutative` /
`assumeNumericAssociative`，浮点重排（加法重结合、乘法链重排）用 `allowFpReassoc`，
特殊值/符号零恒等（`x*0`、`x-x`、`0/x`、`x/x`、`x+0`、`0-x`）用 `allowUnsafeFpIdentities`。

## 测试

`make test` 执行 `tests/run_tests.sh`：

| 阶段 | 检查内容 |
|------|----------|
| 代价回归 | `tests/fixtures/` 下各夹具的 FLOP 计数，分三档执行（默认 / `-r` / `-s`） |
| 数值校验 | `verify_*` 编译并运行生成代码（semantics_fixes、equilibrium、safety、recombine、parens、store_aware、float_identities、ref_alias 等） |
| 配置契约 | `verify_config.cpp` / `verify_counter_prop.cpp` 链接 `libcse.a`，钉住 CLI 无法隔离的 `CSEConfig` 开关与插件注入的 pass |
| pass 契约 | `verify_pass_contracts.cpp` 钉住：默认管线每个 pass 必须能触发、只有 LoopUnroll 可不保效果、跨语句 pass 必须声明写集契约 |
| 形状检查 | 对生成文本的 grep 断言，以及 `check_stmt_switches.py`（语句 switch 必须穷举或带 `// partial:` 标记） |
| `csegen` 冒烟 | 生成头中出现预期的代表性特化 |
| latset 表防漂移 | 引擎表 vs FreeLB `lattice_set.h`（无 FreeLB checkout 时跳过） |
| FreeLB 验证脚本 | `verify_{moment,equilibrium,force}.py`（无 FreeLB checkout 时跳过） |

代价模型按**标量 FLOP、向量按 lane 数加权**计费（向量点积为 `2d-1`），已知纯
helper 经 FreeLB 钩子计价（`getnorm2` → `2d-1` 等），DAG 子表达式按**引用次数**
计费——每个使用点一次（codegen 每次重印都计），物化临时只算定义一次，
`totalNodes` 仍为唯一节点数。无法解析的构造显式报告 `unknownLoops` /
`unmodeledCalls`（此时 flops 是下界）。详见 `docs/cost_model_redesign.md`。

示例结果（before → after）：`basic_cse` 11 → 10、`features` 52 → 51、
`equilibrium_d3q19` 323 → 84 FLOPs（−74.0%）、`safety_cases` 24 → 21、
`cost_nested` 478 → 355（`const` 形参不再视为只读根后的保守计数）、
`cost_descending` 35 → 35。

## 仓库结构

```
src/            通用前端、IR、pass、后端与代价模型
plugins/freelb/ FreeLB 配置钩子、latset 解析、.ur.h 发射器，
                以及两个程序入口
tests/fixtures/ //@cse 输入，绑定期望的 FLOP 计数
tests/verify/   数值验证器与 latset 表防漂移检查
tests/csegen/   .ur.h 生成冒烟输入
docs/           架构文档与移植状态
```

## 文档

- [docs/architecture.md](docs/architecture.md) — 流水线、IR、pass、安全模型、
  构建与测试覆盖
- [docs/cost_model_redesign.md](docs/cost_model_redesign.md) — 代价口径
  （按引用计费、向量加权、钩子）
- [docs/freelb_port_status.md](docs/freelb_port_status.md) — FreeLB 集成状态、
  未完成 TODO 与通用/FreeLB 边界
- [docs/cse-round6-audit.md](docs/cse-round6-audit.md) — 最近一轮全仓库架构
  审计（缺陷、修复、backlog 处置；不入版本控制）

## 已知局限

不做类型检查与模板实例化（类型以字符串表示），解析器无错误恢复，优化仅限标记区域，
只展开常量边界的计数循环。完整列表见
[docs/architecture.md](docs/architecture.md)。

## 许可证

GNU General Public License v3.0，详见 [LICENSE](LICENSE)。
