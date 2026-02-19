# EWSS vs Simple-WebSocket-Server 性能对比方案

**日期**: 2026-02-18
**状态**: P2 性能优化任务设计文档

---

## 1. 项目对标分析

### Simple-WebSocket-Server 特点

| 特性 | SWS | 说明 |
|------|-----|------|
| **语言** | C++11 | 较新但仍被支持 |
| **依赖** | ASIO (Boost) | 重量级库，提供多种后端 |
| **架构** | 多线程 Reactor | 线程池处理连接 |
| **内存** | 动态分配 | std::string, std::vector |
| **异常** | 启用 | 完整异常处理 |
| **目标** | 通用 WS 服务器 | 跨平台，功能完整 |

### EWSS 特点

| 特性 | EWSS | 说明 |
|------|------|------|
| **语言** | C++17 | 现代化，constexpr 友好 |
| **依赖** | sockpp (轻量级) | 仅 socket 封装 |
| **架构** | 单线程 Reactor | poll/epoll，无线程开销 |
| **内存** | 静态分配 | FixedVector, RingBuffer |
| **异常** | 可禁用 | `-fno-exceptions` 支持 |
| **目标** | 嵌入式优先 | ARM-Linux，确定性 |

---

## 2. 性能对比维度

### 2.1 吞吐量 (Throughput)

**场景**: 1000 个并发连接，每秒发送 1 条消息

```
指标:
  - 消息/秒 (messages/sec)
  - 字节/秒 (throughput)
  - 延迟分布 (p50, p99, p99.9)

测试:
  1. 小消息 (10 字节)
  2. 中消息 (1 KB)
  3. 大消息 (100 KB)
  4. 混合负载 (20% 小 + 50% 中 + 30% 大)
```

### 2.2 内存占用 (Memory)

**场景**: 稳定状态，1000 个连接

```
指标:
  - RSS (Resident Set Size)
  - 堆用量 (heap allocation)
  - 每连接内存
  - 内存碎片度 (fragmentation)

测试:
  1. 空闲状态
  2. 10% 流量
  3. 50% 流量
  4. 100% 流量
  5. 峰值后恢复
```

### 2.3 CPU 占用 (CPU Usage)

**场景**: 1000 连接，不同消息速率

```
指标:
  - CPU% (user + system)
  - 上下文切换数
  - 缓存命中率
  - 功耗 (mW，嵌入式关键)

测试:
  1. 1 msg/sec/conn
  2. 10 msg/sec/conn
  3. 100 msg/sec/conn
  4. 1000 msg/sec/conn (峰值)
```

### 2.4 延迟分析 (Latency)

**场景**: 端到端消息往返时间（RTT）

```
指标:
  - P50 延迟 (中位数)
  - P95 延迟
  - P99 延迟 (尾延迟，关键)
  - Max 延迟
  - 延迟稳定性 (方差)

测试:
  1. 正常负载 (1000 conn, 1 msg/sec)
  2. 高负载 (1000 conn, 100 msg/sec)
  3. 峰值负载 (1000 conn, 1000 msg/sec)
```

### 2.5 可扩展性 (Scalability)

**场景**: 增加连接数，观察性能变化

```
测试连接数:
  - 100
  - 500
  - 1000
  - 5000
  - 10000 (如果支持)

关键指标:
  - 吞吐量缩放比例 (linear? sublinear?)
  - 延迟增长趋势
  - CPU 增长趋势
  - 内存线性度
```

### 2.6 可靠性 (Reliability)

**场景**: 长时间运行，故障恢复

```
测试:
  1. 1 小时连续运行
  2. 内存泄漏检测 (valgrind)
  3. 网络错误恢复
  4. 连接断开后重连
  5. 缓冲区溢出处理
```

---

## 3. 测试环境规范

### 硬件

| 项 | 规格 | 说明 |
|----|------|------|
| **CPU** | Intel Xeon / ARM (dual) | 代表服务端/嵌入式 |
| **内存** | 4GB 最小 | 观察内存差异 |
| **网络** | 1Gbps + local loopback | 消除网络变量 |

### 软件

```
操作系统: Linux (Ubuntu 22.04 LTS / 嵌入式 ARM)
编译器:
  - SWS: GCC 11, Clang 14
  - EWSS: GCC 11, Clang 14
编译标志:
  - SWS: -O3 (标准优化)
  - EWSS: -O3 -march=native (对标公平)
```

### 工具链

```
性能测试:
  - 自写 benchmark 工具 (使用 steady_clock)
  - 客户端: WebSocket 压测工具 (artillery, wscat-bench)

监测工具:
  - /proc/self/stat - CPU, context switch
  - /proc/self/status - 内存
  - perf (CPU profiling)
  - valgrind (内存检测)
  - flamegraph (火焰图)
```

---

## 4. 基准测试程序框架

### 4.1 服务器端程序

```cpp
// EWSS benchmark server
// benchmarks/ewss_bench_server.cpp

#include "ewss/server.hpp"
#include <chrono>

struct BenchStats {
  uint64_t messages_received = 0;
  uint64_t bytes_received = 0;
  uint64_t total_latency_ns = 0;  // 从消息到达到响应发送
  std::chrono::steady_clock::time_point start_time;
};

int main() {
  ewss::Server server(8080);
  BenchStats stats;
  stats.start_time = std::chrono::steady_clock::now();

  server.on_message = [&](const auto& conn, std::string_view msg) {
    auto now = std::chrono::steady_clock::now();
    stats.messages_received++;
    stats.bytes_received += msg.size();

    // Echo back immediately
    conn->send(msg);
  };

  server.run();
}
```

### 4.2 客户端压测工具

```bash
# benchmarks/ws_load_test.sh

#!/bin/bash

# 参数
CONNECTIONS=1000
RATE=1  # msg/sec/conn
DURATION=60  # seconds

# 启动 N 个客户端连接
for i in $(seq 1 $CONNECTIONS); do
  (
    while true; do
      wscat -c ws://localhost:8080 -m "bench_$(date +%s%N)" 2>/dev/null
    done &
    sleep $((RATE))
  ) &
done

# 收集指标
watch -n 1 'ps aux | grep server | grep -v grep'
```

### 4.3 数据采集脚本

```python
# benchmarks/collect_metrics.py

import subprocess
import json
import time
from pathlib import Path

def collect_cpu_memory():
    """Collect CPU and memory metrics every 100ms"""
    result = subprocess.run(
        ['cat', '/proc/self/stat'],
        capture_output=True, text=True
    )
    # Parse stat file to extract CPU time, RSS, etc.
    pass

def run_benchmark(server_cmd, duration_sec=60):
    """Run server and collect metrics"""
    start = time.time()
    metrics = []

    while time.time() - start < duration_sec:
        metrics.append(collect_cpu_memory())
        time.sleep(0.1)

    return metrics

# Output as JSON for analysis
```

---

## 5. 性能对比预期

### 理论分析

| 维度 | EWSS 优势 | SWS 优势 | 预期赢家 |
|------|---------|---------|---------|
| **吞吐量** | 单线程，无锁竞争 | 多线程可利用多核 | SWS (多核场景) |
| **延迟** | 固定 buffer，无锁 | 锁竞争延迟 | EWSS (特别是尾延迟) |
| **内存** | 静态分配 | 动态分配 + ASIO 开销 | EWSS (5-10x 优势) |
| **功耗** | 最小化 | ASIO 后台线程 | EWSS (显著) |
| **可预测性** | 确定性路径 | 锁争用不确定 | EWSS |
| **扩展性** | 单核优化 | 多核扩展好 | SWS (10核+) |

### 预期数据范围

```
吞吐量:
  - SWS: 100K-200K msg/sec (多线程优势)
  - EWSS: 50K-100K msg/sec (单线程)

延迟 (P99):
  - SWS: 5-50ms (锁竞争影响)
  - EWSS: <1ms (无锁设计)

内存 (1000 conn):
  - SWS: 200-300 MB
  - EWSS: 15-20 MB (10-15x 优势)

功耗:
  - SWS: ~5W (多个线程)
  - EWSS: ~1W (单线程，ARM)
```

---

## 6. 报告输出格式

### 6.1 汇总表格

```
┌─────────────────────┬──────────────┬──────────────┬──────────┐
│ 指标                 │ SWS          │ EWSS         │ 赢家     │
├─────────────────────┼──────────────┼──────────────┼──────────┤
│ Throughput (msg/s)  │ 150,000      │ 75,000       │ SWS 2x  │
│ P99 Latency (ms)    │ 25           │ 0.5          │ EWSS 50x│
│ Memory (MB)         │ 250          │ 18           │ EWSS 14x│
│ CPU (1000 conn)     │ 45%          │ 12%          │ EWSS 4x │
│ Tail latency spike  │ 显著         │ 最小         │ EWSS    │
│ 内存碎片化          │ 显著         │ 无           │ EWSS    │
│ 多核扩展            │ 优秀         │ 有限         │ SWS     │
│ 功耗 (ARM)          │ 2.5W         │ 0.8W         │ EWSS 3x │
└─────────────────────┴──────────────┴──────────────┴──────────┘
```

### 6.2 火焰图对比

```
SWS 火焰图:
  - 栈深: 20-30 层 (ASIO 库调用链长)
  - 热点: pthread_mutex_lock (锁竞争)
  - malloc/free (频繁分配)

EWSS 火焰图:
  - 栈深: 5-8 层 (直接调用)
  - 热点: poll (I/O 等待)
  - 内存操作 (仅初始化)
```

### 6.3 可视化图表

```
1. 吞吐量 vs 连接数 (折线图)
2. 延迟分布 (直方图，P50/P95/P99)
3. 内存增长曲线
4. CPU 使用率趋势
5. 功耗对比 (柱状图)
```

---

## 7. 实施计划

### Phase 1: 基础设施 (P2.1, 1-2h)
- [ ] 编写 EWSS benchmark 服务器
- [ ] 编写 SWS benchmark 适配器
- [ ] 客户端压测工具
- [ ] 数据采集脚本

### Phase 2: 测试执行 (P2.2, 2-3h)
- [ ] 运行吞吐量测试
- [ ] 运行延迟测试
- [ ] 运行内存/CPU 测试
- [ ] 运行可靠性测试

### Phase 3: 分析与报告 (P2.3, 1-2h)
- [ ] 数据清理和分析
- [ ] 生成火焰图
- [ ] 编写对比报告
- [ ] 创建可视化图表

### Phase 4: 优化建议 (P2.4, 1h)
- [ ] 识别 EWSS 瓶颈
- [ ] 提出优化方向
- [ ] 评估改进潜力

**总耗时**: 5-8 小时（P2 主要工作）

---

## 8. 性能优化机会

基于预期，EWSS 可在以下方面进一步优化：

### 8.1 多核支持 (可选)
```
方案: 分片 Reactor
  - N 个独立的 poll 循环 (一个/核心)
  - 无锁分发 (连接 hash → shard)
  - 期望: 吞吐量 2-4x 提升
```

### 8.2 SIMD 优化 (可选)
```
方案: 批量帧处理
  - SSE/AVX 解掩码
  - 批量校验和计算
  - 期望: 延迟 10-20% 改进
```

### 8.3 TCP 优化
```
方案:
  - TCP_NODELAY (禁 Nagle)
  - SO_SNDBUF/SO_RCVBUF 调优
  - 期望: 延迟 5-10% 改进
```

---

## 9. 结论模板

性能对比完成后，报告会包括：

1. **EWSS 优势区间**: 延迟、内存、功耗、可预测性
2. **SWS 优势区间**: 吞吐量、多核扩展
3. **适用场景**:
   - **EWSS**: 嵌入式、低功耗、实时性要求
   - **SWS**: 高吞吐量、多核服务器、传统企业应用
4. **建议**: 根据场景选择

---

**下一步**: 等待 P1.3 & P1.4 完成，进入 P2 性能优化阶段

EOF
cat > /tmp/ewss/docs/PERFORMANCE_COMPARISON_PLAN.md << 'EOF'
# 详见上方完整内容
EOF
