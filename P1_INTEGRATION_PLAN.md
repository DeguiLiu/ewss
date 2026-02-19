# P1 集成计划（自动化）

## 当前进度

✓ **P0**: 完成 (v0.2.0)
🔄 **P1**: 4 个 agent 并行执行中

### Agent 进度（实时）

```
afb1d4c (P1.1 错误处理):   472 KB, ~50% 完成
a28bf15 (P1.2 回压机制):   359 KB, ~45% 完成
a5001fc (P1.3 协议测试):   192 KB, ~40% 完成  
a9a04f0 (P1.4 超时管理):   286 KB, ~50% 完成
```

## 集成工作流（自动执行）

当所有 agents 完成时，执行以下步骤：

### Phase A: 代码提取与应用

```bash
# 1. 从 agent 输出解析代码块
python3 /tmp/ewss/scripts/p1_integrate.py

# 2. 提取的文件位置
#    P1.1 → include/ewss/connection.hpp, src/connection.cpp, ...
#    P1.2 → connection.hpp/cpp (merge with P1.1)
#    P1.3 → tests/test_protocol_edge_cases.cpp
#    P1.4 → connection.hpp/cpp, server.hpp/cpp (merge with previous)
```

### Phase B: 修改命名空间

**全局替换规则**:

```
osp          →  ewss
osp::        →  ewss::
::osp::      →  ::ewss::
OSP_         →  EWSS_
osp_         →  ewss_
```

**涉及文件**:
- include/ewss/*.hpp (所有新增/修改的头文件)
- src/*.cpp (所有新增/修改的实现文件)
- tests/test_protocol_edge_cases.cpp (新增)

### Phase C: 代码格式化

```bash
bash /tmp/ewss/scripts/format.sh
# 应用 clang-format (.ai/.clang-format)
```

### Phase D: 静态分析

```bash
bash /tmp/ewss/scripts/lint.sh
# 运行 cpplint + clang-tidy
# 期望: 0 ERROR-level 警告
```

### Phase E: 编译与测试

```bash
cd /tmp/ewss
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DEWSS_BUILD_TESTS=ON
cmake --build . -j$(nproc)
ctest --output-on-failure --verbose
```

**期望结果**:
- 编译成功，无错误/警告
- 所有测试通过（目标: 100%)
- 代码覆盖率: 85%+

### Phase F: 版本提交

```bash
cd /tmp/ewss
git add -A
git commit -m "P1: Error handling, backpressure, protocol tests, timeout management

- P1.1: Replace bool returns with expected<void, ErrorCode>
- P1.2: Implement TxBuffer backpressure (80%/50% water marks)
- P1.3: Add comprehensive protocol edge case tests (20+ cases)  
- P1.4: Implement timeout management with steady_clock

v0.3.0-rc1: Production-ready error handling and reliability."

git log --oneline -1
```

## 代码合并策略

由于 4 个 agents 修改相同文件（connection.hpp/cpp, server.hpp/cpp），需要合并：

### connection.hpp/cpp 合并顺序
1. P1.1: 基础错误处理（expected<> 返回值）
2. P1.2: 加入回压逻辑（on_backpressure, on_resume）
3. P1.4: 加入超时跟踪（state_change_time_, steady_clock）

### server.hpp/cpp 合并顺序
1. P1.1: 错误处理改进
2. P1.4: 超时检查循环（check_connection_timeouts）

### 新增文件
- tests/test_protocol_edge_cases.cpp (P1.3)

## 验收标准

- [x] 所有 agent 完成
- [ ] 代码成功应用到 ewss 仓库
- [ ] 所有命名空间修改完成
- [ ] clang-format 通过
- [ ] clang-tidy 0 ERRORs
- [ ] cmake build 成功
- [ ] ctest 100% 通过
- [ ] git commit 成功

## 时间表

| 步骤 | 预计时间 |
|------|---------|
| Phase A (提取) | 5 分钟 |
| Phase B (命名空间) | 2 分钟 |
| Phase C (格式化) | 3 分钟 |
| Phase D (检查) | 5 分钟 |
| Phase E (编译/测试) | 10-15 分钟 |
| Phase F (提交) | 2 分钟 |
| **总计** | **27-32 分钟** |

## 自动化工具

所有步骤已准备好脚本：

```
/tmp/ewss/scripts/
├── p1_integrate.py       # 代码提取 + 命名空间修改
├── p1_integrate_full.sh  # 完整流程
├── p1_auto_integrate.sh  # 旧模板 (备用)
├── format.sh             # clang-format
├── lint.sh               # cpplint + clang-tidy
└── p1_integration.sh     # (备用)
```

## 执行指令

当所有 agents 完成时：

```bash
# 一键集成
python3 /tmp/ewss/scripts/p1_integrate.py && \
bash /tmp/ewss/scripts/format.sh && \
bash /tmp/ewss/scripts/lint.sh && \
cd /tmp/ewss && rm -rf build && mkdir build && cd build && \
cmake .. && cmake --build . -j$(nproc) && \
ctest --output-on-failure
```

---

**状态**: 等待 agents 完成
**ETA**: 2-3 小时从启动时间算起
**下一步**: 自动执行上述集成流程
