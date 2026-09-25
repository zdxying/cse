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
  → [Reassociate] → CSEPass → [ExprRecombine → AlgebraicSimplify] → ValueProp → DCE
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
| `-c`, `--cost` | 打印优化前后的 FLOP / 节点 / 语句 / 变量计数 |
| `--json` | 以 JSON 输出代价报告（配合 `-c`） |
| `-r`, `--recombine` | 启用提取公因子（`a*x + a*y → a*(x+y)`） |
| `-s`, `--safe` | 面向任意 C++ 的保守语义 |
| `-v`, `--verbose` | 逐 pass 打印到 stderr |
| `-h`, `--help` | 用法 |

### 生成 FreeLB 的 `.ur.h` 特化

```bash
./bin/csegen tests/csegen/moment.h /tmp/moment.ur.h
```

`csegen <input.h> <output.h>` 读取带 `// @cse` 标记的生产头文件，为每个受支持的格子集
（D2Q5、D2Q9、D3Q7、D3Q15、D3Q19、D3Q27）发射完全展开的特化。输入文件的 basename
必须是 `moment`、`equilibrium` 或 `force` 之一。

FreeLB 通过 `third_party/cse` submodule 使用本工具：集成方式、构建入口与当前状态见
[docs/freelb_port_status.md](docs/freelb_port_status.md)。

## 安全模型

在保守模式下（`-s`，也是 `CSEConfig` 的默认值），优化通过五条机制保证行为等价：

1. **纯度**：调用默认视为有副作用，只有登记为纯的调用才允许去重或跨语句提取。
2. **内存**：只有只读根的 load 可共享；可变/未知根的 load 每次独立，避免跨 store 复用。
3. **支配**：位于 `if`/`else`/循环内的表达式标记为 nested，绝不外提。
4. **作用域**：遮蔽变量做 alpha-rename，同名但无关的声明不会被误合并。
5. **展开前提**：只有当循环体声明的每个局部变量都能被内联消除时才展开，否则保留循环。

代数规则（`a*1`、交换律、重结合）需要 `assumeNumericCommutative` /
`assumeNumericAssociative`，浮点重结合还额外需要 `allowFpReassoc`。

## 测试

`make test` 执行 `tests/run_tests.sh`：

| 阶段 | 检查内容 |
|------|----------|
| 代价回归 | `tests/fixtures/` 下五个夹具的 FLOP 计数 |
| 数值校验 | `verify_equilibrium` 与 `verify_safety` 编译并运行生成代码 |
| `csegen` 冒烟 | 生成头中出现预期的代表性特化 |
| latset 表防漂移 | 引擎表 vs FreeLB `lattice_set.h`（无 FreeLB checkout 时跳过） |
| FreeLB 验证脚本 | `verify_{moment,equilibrium,force}.py`（无 FreeLB checkout 时跳过） |

示例结果：`basic_cse` 11 → 10、`features` 51 → 50、
`equilibrium_d3q19` 228 → 84 FLOPs（−63.2%）、`safety_cases` 20 → 20
（验证等价，而非减少）。

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
- [docs/freelb_port_status.md](docs/freelb_port_status.md) — FreeLB 集成状态、
  未完成 TODO 与通用/FreeLB 边界

## 已知局限

不做类型检查与模板实例化（类型以字符串表示），解析器无错误恢复，优化仅限标记区域，
只展开常量边界的计数循环。完整列表见
[docs/architecture.md](docs/architecture.md)。

## 许可证

GNU General Public License v3.0，详见 [LICENSE](LICENSE)。
