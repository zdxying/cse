# cse 测试集

`make test` 先构建再运行 `tests/run_tests.sh`；`make sanitize` 在 ASan/UBSan
下重建并跑同一脚本。所有生成物写入临时目录，源码树不被修改。

## 三层判据

| 层 | 判据 | 载体 |
|----|------|------|
| 成本回归 | 优化后的 FLOP 数对照固定值，按夹具在 default / `-r` / `-s` 档中钉住 | `run_tests.sh` 的 `cost_stage` |
| 数值差分 | `sf_X` 与未标记的 `ref_X` 孪生在同一 TU 内对同一批输入比返回值 | `tests/verify/verify_*.cpp` |
| 扫描 oracle | 对**全部**夹具的每个 `//@cse` 区域跑：round-trip 保真 / pass 幂等 / 效果保持 / pass 契约 / IR 不变量 | `tests/verify/verify_{roundtrip,ir_idempotence,effect_preservation,pass_contracts,ir_invariants}.cpp` |

前两层按缺陷定制（见下表）；第三层是**通吃**的结构 oracle，新增夹具自动纳入。

## 夹具 → 缺陷 → 校验器 → 阶段

| 夹具 | 钉住的缺陷 | 校验器 | 成本阶段 |
|------|-----------|--------|---------|
| `basic_cse` | CSE 基本正确性（共享子表达式） | 孪生差分（`twins/flagship`） | default |
| `features` | 组合特性（括号、共享、循环） | 孪生差分（`twins/flagship`） | default |
| `namespace_case` | `//@cse` 区域含 namespace 时被静默丢弃 | 孪生差分（`twins/flagship`） | default |
| `cost_descending` | 降序计数循环的成本解析（负步长） | `verify_cost_descending` | default |
| `cost_nested` | 嵌套 `dim` 循环 / 未知边界循环的成本 | 孪生差分（`twins/flagship`） | default |
| `safety_cases` | 不纯调用合并、跨 store 复用 load、分支外提、遮蔽、不纯初值循环 | `verify_safety` | default |
| `parens` | codegen 丢括号（右子节点在等优先级未加括号） | `verify_parens` (+`_safe`) | default / `-s` |
| `mixed_ops` | 节点身份漏 `name`/`postfix`（cast、`++`/`--`） | `verify_mixed_ops` (+`_safe`) | default / `-s` |
| `write_visibility` | 写集合不看声明初始化式（`double z = ++y;`） | `verify_write_visibility` (+`_safe`) | default / `-s` |
| `effect_duplication` | `x*2→x+x` 复制有副作用的操作数（含复合操作数） | `verify_effect_duplication` (+`_safe`) | default / `-s` |
| `member_purity` | 与内建数学函数同名的用户成员方法不得判纯（两次调用须各生效） | `verify_member_purity` | default / `-s` |
| `frontend_forms` | 后缀 `i++`、一元 `+`、科学计数法字面量解析 | `verify_frontend_forms` (+`_safe`) | default / `-s` |
| `comment_braces` | 注释/字面量里的 `{}` 使区域边界错位 | `verify_comment_braces` (+`_safe`) | default / `-s` |
| `dead_store_effects` | DCE 删掉值里含 `++`/不纯调用的死 store | `verify_dead_store_effects` (+`_safe`) | default / `-s` |
| `constant_edges` | `a/0` 被折叠成 0、`-0.0` 符号丢失、`1e16` 重拼 | `verify_constant_edges` (+`_safe`) | default / `-s` |
| `void_param` | `f(void)` 空参数表被误解析为参数 | `verify_void_param` (+`_safe`) | default / `-s` |
| `incdec_safety` | `--x` 被当成 `-x`（自减被代数/重组 pass 抹掉） | `verify_incdec_safety` (+`_safe`) | default / `-s` |
| `loop_bound` | 非整边界 `i<4.5` 被 `static_cast<int>` 截断、丢迭代 | `verify_loop_bound` (+`_safe`) | default / `-s` |
| `loop_unroll_semantics` | 复合算子丢失、load 被移过 store | `verify_loop_unroll_semantics` (+`_safe`) | default / `-s` |
| `value_prop_chain` | 平凡局部链内联后留下悬垂引用 | `verify_value_prop_chain` (+`_safe`) | default / `-s` |
| `cleanup_store_index` | 仅作 store 下标的声明被删 | `verify_cleanup_store_index` (+`_safe`) | default / `-s` |
| `cleanup_nested` | 嵌套块（`if`/`for`）内的死声明、自赋值须被清理 | `verify_cleanup_nested` (+`_safe`) + 形状检查 | default / `-s` |
| `loop_control` | 花括号块内的 `break;`/`continue;` 不得被当死代码删除 | 孪生差分 + 形状检查 | default / `-s` |
| `ref_param_store` | DCE 删掉对引用的 store（写调用者对象） | `verify_ref_param_store` (+`_safe`) | default / `-s` |
| `loop_unroll_inline` | 循环体局部的初值被内联到它读的变量之后 | `verify_loop_unroll_inline` (+`_safe`) | default / `-s` |
| `loop_unroll_chain` | 循环展开的内联须到不动点：未收敛会留下对已删声明的引用而编译失败 | `verify_loop_unroll_chain` (+`_safe`) | default / `-s` |
| `loop_escape` | 循环变量在循环外被读时不得展开（对照：受控计数器、同名形参、兄弟循环仍展开） | 孪生差分（`twin_run`）+ 形状检查 | `-s` |
| `dce_nested` | DCE 必须递归进入 `if`/`for` 嵌套块 | 孪生差分（`twin_run`） | `-s` |
| `cleanup_chain_effects` | Cleanup 链式合并不得融合不纯的追加项，也不得融合与累加器别名的引用形参 | `verify_cleanup_chain_effects` (+`_safe`) | default / `-s` |
| `fresh_names` | 生成名（`_cse_*`、`*__u*`、`*__s*`）不得与用户标识符撞名 | 孪生差分（`twin_run`） | — |
| `shadow_global` | region 读而未声明的外部名不得与内部同名声明共用节点 | 孪生差分（`twin_run`） | — |
| `speculation` | 守卫内的除法/越界读不得被提出条件之外（`k ? 1/k : 0` 不得在 `k == 0` 时求值） | 孪生差分（`twin_run`） | `-s` |
| `numeric_kind` | 丢掉唯一的浮点操作数、或把浮点表达式换成整型常量，会静默改变 C++ 类型 | `verify_numeric_kind` (+`_safe`) | default / `-s` |
| `purity_names` | 内建纯函数白名单只认限定名（用户自定义 `max` 的两次调用须各生效） | `verify_purity_names` + 形状检查 | default |
| `struct_region` | 纯数据 struct 区域被发射为空 | `verify_struct_region` | default / `-s` |
| `region_toplevel` | 顶层声明被吞进下一个函数区域并丢弃 | `verify_region_toplevel` | default / `-s` |
| `semantics_fixes` | 逻辑算子、括号、复合 store、不纯调用、整除、遮蔽、链合并、有符号零、分数循环起点 | `verify_semantics_fixes` (+`_safe`) + 孪生差分 | default / `-s` / `-r` |
| `chained_accum` | CleanupPass 合并加法链（也是 pass 契约 oracle 的"点火"夹具） | 形状检查（`run_tests.sh`） | default / `-s` |
| `equilibrium_d3q19` | D3Q19 二阶平衡态基准（数值精度） | `verify_equilibrium` | default |
| `float_identities` | 保守档必须保留 IEEE 身份（`x/x`、`x*0`、有符号零） | `verify_float_identities` | `-s` |
| `exact_identities` | 保守档**保留**逐位精确的恒等式（`x*1`、`x-0`、`x*2`、`(-a)*(-a)` 等），并钉住符号规范化的唯一例外（`0*inf` 的 NaN 符号位） | `verify_exact_identities` + 形状检查 | `-s` |
| `const_typing` | 常量的 int/float 类型须跨折叠与去重保留（`4.0/3` ≠ `4/3`） | 孪生差分 | default / `-s` |
| `zero_product_kind` | 零恒等式替换出的常量须携带表达式的 int/float kind（`(0*x+2)/4` 在 double 下是 0.5、int 下是 0） | `verify_zero_product_kind` + `verify_zero_product_activity` | default / `-s` |
| `write_set_shapes` | 写集缺陷类：for-init 引用、引用链、值不得跨操作数写复用 | 孪生差分 + `verify_write_set` | — |
| `ref_rebind` | 引用的 referent 被内联进绑定（`double& r = 0.5`，不合法）、经引用的读跨 referent 写被复用 | 孪生差分 | — |
| `store_aware` | 跨语句重写不得越过 store（CSE/ValueProp） | `verify_store_aware` (+`_safe`) | `-s` |
| `ref_alias` | `const T&` 参数与 `const T*` 一样可别名 | `verify_ref_alias` | `-s` |
| `recombine` | `-r` 的乘法因子分解 / 除法不得合并 / cast 身份 | `verify_recombine` | `-r` |
| `pp_directives` | 预处理指令不得被丢弃（`#if 0` 体复活）；注释/字符串里的 `#` 不是指令 | `verify_pp_directives` + 形状检查 | — |
| `long_chain` | 60 级定义链：pass 必须收敛到不动点，不得受固定轮数限制 | `verify_long_chain` + 形状检查（残留声明数、二次运行不变） | — |
| `effect_order` | 重结合不得把带效果项移过对它所写状态的读（`state + f() + state` 不得变成 `state + state + f()`） | `verify_effect_order` + 形状检查（效果链保序、纯链仍重排） | `-r` |
| `kind_through_unroll` | 循环展开重命名局部后必须保留数值 kind（`v/v` 在循环内外同样折叠） | `verify_kind_through_unroll` + 形状检查 + `verify_ir_invariants` 类级判据 | — |
| `frontend_gaps` | 四类整区域不可解析的前端缺口：`size_t` 作 for-init 类型、顶层 `static inline`、`<<`/`>>`（相邻 `<` `<` 识别，模板 `>>` 不受影响）、一元 `*p`（降级为 `p[0]`） | `verify_frontend_gaps` + 形状检查（区域必须被优化，不是透传） | — |
| `effect_for_update` | for-update 槽推进非计数器变量（`++g`）是真实效果且值不变 | 孪生差分（`twin_run`）+ 扫描 oracle | — |
| `bitwise_ops` | 位算子（`\|` `^` `~` `%=` `&=` …）词法/优先级 | 见下 | — |

`bitwise_ops` 不钉成本，由 `verify_lexer`（token 普查：无字节被静默丢弃、多字符算子
单 token）与第三层扫描 oracle 覆盖。`pp_directives` 未登记成本钉，成本阶段为 `—`；
其判据是 `verify_pp_directives` 与"预处理器指令"形状检查（含"仅注释里出现 `#`
的区域必须被优化"）。

## 校验器分类

- **缺陷钉**（按夹具定制，见上表）：`#include "X.cpp.cse"`，比 `sf_`/`ref_` 的值。
  多数在 default 与 `-s` 两档各编译一次（`_safe` 变体，用 `-I` 指向该档输出目录）。
- **扫描 oracle**（5 个）：遍历 `tests/fixtures/*.cpp`，逐区域判结构性质，不依赖具体缺陷。
- **库级**：`verify_config`（`CSEConfig` 契约，CLI 够不到的开关）、`verify_builder_guards`
  （空语句槽位的 builder 守卫，parser 保证不可达）、`verify_counter_prop`（`CounterPropPass`
  的嵌套作用域 copy-in 种子：CLI 不注入该 Pass，故直接驱动它，判别循环自写计数器不再被误折）、
  `verify_write_set`（写集**类级**判据：按形状表断言闭包后的写集根集合，覆盖 for-init 引用、
  decl-init、元素 store 下标、引用链、不纯调用通配；形状表是人工枚举的，新增形状需加一行）、
  `verify_purity`（纯度问题的浅/深区分：`isReferentiallyTransparent` 与节点 `pure` 标志）、
  `verify_pp_extraction`（预处理指令判定：`TokenType::PreprocessorDirective` 与
  `CSERegion::hasDirectives`，区分注释后的 `#if` 与注释/字符串里的 `#`）、
  `verify_node_identity`（节点身份表 `CSE_DAGNODE_IDENTITY_FIELDS` 的漂移守卫：凡改变节点语义的字段，
  都必须让两个节点 `sameContentAs` 为假**且** hash 相异——从宏里删掉一个字段，该 verifier 即红并点名该字段）、
  `verify_zero_product_activity`（零恒等式**是否真的触发**：值判据对"没做"是失明的，不折叠同样得到正确结果，
  故直接数优化后 IR 里的乘节点，并断言 `assumeFloatOperands` 关闭时整型字面量那条仍保留）、
  `verify_cost`（成本口径本身：按引用次数计费而非唯一节点，在两者结果不同的形状上钉住）。
- **词法**：`verify_lexer`（token 普查；另以 `static_assert` 钉住 `Lexer`/`Parser` 不得由临时量构造——
  它们的输入是借用的，右值重载已 `delete`）。
- **孪生差分**：`run_tests.sh` 的 `twin_run` 对表中标注孪生差分的夹具（以及
  `tests/verify/twins/ref_write.cpp`、`tests/verify/twins/flagship.cpp`）用 `gen_twin_main.py`
  生成 `main`，在 `-s` 档对同一批输入比 `sf_`/`ref_`（引用局部写必须使其 referent 失效；
  旗舰夹具以标量形状覆盖 `basic_cse`/`features`/`namespace_case`/`cost_nested`；写集与引用
  读/重绑定形状见上表）。比较是**逐位精确**的：`-s` 档不得重结合浮点，容差会掩盖小误编译。
  `twin_run` 断言可比对数下限，无配对即失败（不得以 `SKIPPED` 静默通过）。
- **生成式差分模糊**（`gen_shapes.py`）：从写集/引用形状的文法**按种子确定性**抽程序，同一孪生差分
  逐位比对。它不依赖人先把形状写进夹具——分析器漏枚举一种形状，或某 pass 跨它没看见的写复用了一个值，
  会以编译失败或数值不符自行浮现。`run_tests.sh` 默认跑种子 1..32（`CSE_FUZZ_SEEDS` 可覆盖），
  并断言"多数种子确实被优化"以防空转；随后再断言文法仍生成数组、元素 store 与非计数器推进。
- **Python**：`gen_twin_main.py`（生成孪生 `main`）、`gen_shapes.py`（确定性生成式孪生程序）、
  `check_lattice.py`（引擎硬编码
  lattice 表 vs FreeLB `src/lbm/lattice_set.h` 的漂移守卫，需 FreeLB checkout）、
  `check_self_assign.py`（csegen 冒烟中对生成 `.ur.h` 断言无 `a[i] = a[i];`，注释盲）、
  `check_stmt_switches.py`（`StmtIRKind` 的 switch 须穷举，或在 `default:` 行标 `// partial:`）。

## run_tests.sh 阶段

1. `cost regression` default / `-r` / `-s`：三档 FLOP 钉（`pp_directives` 无钉，见上）。
2. 形状检查（grep 生成文本）：`-r` 因子分解与混合类型拒绝、`-s -r` 重组许可、`x*2→x+x`、
   精确恒等式档、死 store、常量发射、store 下标、store-aware、引用 store、循环内联改名、
   CleanupPass 链及 `-s` 保守性、嵌套清理、链式效果保序、`break`/`continue` 保留、
   循环计数器逃逸、限定名纯度白名单、`<`/`>` 解析后仍优化、预处理指令保真、
   长链收敛（残留声明数为 0 且二次运行输出不变）、效果保序、重命名后 kind 仍在。
3. 深度与错误隔离：表达式嵌套上限（超限区域透传、`CSE-PARSE-DEPTH`、退出 3）；
   `error containment`：一个区域失败不得拖垮整文件（退出 2），全部失败退出 3 且仍产出。
4. 数值校验器（default + `-s` + `-r`），含预处理指令区域的双档验证。
5. 库级：config 契约、counter-prop 嵌套作用域、builder 守卫、零恒等式触发面、成本口径规则、
   写集形状、纯度问题、预处理指令提取、节点身份表。
6. 扫描 oracle：round-trip / 幂等 / 效果保持 / pass 契约 / IR 不变量（含 `bin/cse --verify`）。
7. 词法普查、区域覆盖上报、outcome 契约、marker 鲁棒性、token 边界、孪生差分。
8. 生成式差分模糊（`-s`，种子默认 1..32）：按种子生成程序 → 优化 → 编译 → 孪生逐位比对，
   并断言多数种子确实被优化；随后检查文法仍生成各类形状。
9. 生成物可编译（每个夹具过 `g++ -fsyntax-only`，`features` / `cost_nested` 显式豁免）；
   `StmtIRKind` switch 穷举检查。
10. `csegen` 冒烟（`tests/csegen/*.h` → `.ur.h`，覆盖率 0 skip）、自赋值守卫、
    指令区域拒绝、`--cost` 报告。
11. FreeLB（可选，需 checkout）：lattice 漂移守卫 + `.ur.h` 校验。

## 约定

- `//@cse` 标记区域；`// @cse`（带空格）同样识别，行尾文本原样保留。
- 孪生：标记的 `sf_X` 与未标记的 `ref_X` 同体；`sf_` 被优化、`ref_` 原样拷贝，二者比数值。
- 成本口径是"**按引用次数**（每个使用点一次）"，不是 DAG 唯一节点数；物化临时只算其
  定义一次（见 `run_tests.sh` 注释，规则本身由 `tests/verify/verify_cost.cpp` 钉住）。
- 新增缺陷钉：夹具 + 校验器（用 `tests/verify/harness.h`）+ 一行 `run_verifier` 登记。
