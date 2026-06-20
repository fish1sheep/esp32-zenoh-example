# z_pull.md — ESP32 (S3 / C5) Zenoh 拉取式订阅者（环形通道）教程

[← 返回 docs](../README.md)

---

## 概述

`main/z_pull.c` 是一个面向 **ESP32 (S3 / C5)** 的 Zenoh **拉取式订阅者（Pull Subscriber）** 示例程序，运行于 **ESP-IDF v6.0** 框架之上。它演示了如何使用**环形通道（Ring Channel）**——一个有界 FIFO 缓冲区——让应用程序按自己的节奏接收发布数据，而非通过即时回调。

### 拉取 vs. 回调订阅者

Zenoh 支持两种订阅者模型：

| 对比维度 | 回调订阅者 (`z_sub.c`) | 拉取订阅者 (`z_pull.c`) |
|---------|----------------------|------------------------|
| 接收方式 | Zenoh 立即调用你的处理器 | 你自己调用 `z_try_recv()` |
| 并发 | 处理器在 Zenoh 内部上下文中运行 | 在主循环中拉取 |
| 缓冲 | 无缓冲（必须在处理器中立即处理） | 环形通道可缓存最多 N 个样本 |
| 适用场景 | 实时响应、低延迟 | 固定周期轮询、定期批处理 |
| 溢出处理 | 同步（无溢出） | 环形满时丢弃最旧样本 |

### 环形通道工作原理

```
[发布者] → 发布消息
       │
       ▼
[Zenoh 会话] 接收并反序列化
       │
       ▼
[闭包]（秘密写入环形缓冲区）
       │
       ├─ 有空位 → 存储样本
       └─ 环形已满 → 丢弃最旧样本
       │
       ▼
[处理器] — 应用程序调用 z_try_recv() 取出
       │
       ├─ z_try_recv 返回 Z_OK → 有可用样本
       └─ z_try_recv 返回 Z_CHANNEL_NODATA → 缓冲区为空
```

环形通道创建为一对：
- **闭包（Closure）** — 传给 `z_declare_subscriber()`；Zenoh 向其写入
- **处理器（Handler）** — 应用程序通过 `z_try_recv()` 从中读取

### 核心功能

| 功能 | 说明 |
|------|------|
| WiFi STA 连接 | 连接指定 SSID，自动重试 |
| 环形通道订阅者 | 最多缓冲 3 个样本，溢出时丢弃最旧 |
| 定期轮询 | 每 5 秒检查一次环形通道 |
| 非阻塞接收 | `z_try_recv()` 绝不阻塞——立即返回 |
| 内存安全 | 每个样本处理后正确释放 |

### 数据流

```
[对等端或路由器]              [ESP32 (S3 / C5) 拉取订阅者]
      │                               │
      │  pub "demo/example/foo"       │
      │ ─────────────────────────────→│
      │                               │  环形: [sample1]
      │  pub "demo/example/bar"       │
      │ ─────────────────────────────→│
      │                               │  环形: [sample1] [sample2]
      │                               │
      │                          (每 5 秒)
      │                               │
      │                               │  z_try_recv() → sample1
      │                               │  z_try_recv() → sample2
      │                               │  z_try_recv() → Z_CHANNEL_NODATA
      │                               │  → 休眠 5 秒
      │                               ▼
      │                         串口控制台
```

---

## 前置条件

### 硬件

- ESP32 开发板（ESP32-S3-DevKitC-1 或 ESP32-C5-DevKitC）
- USB-C 数据线

### 软件

| 工具 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | v6.0.1 | 嵌入式开发框架 |
| zenoh-pico | v1.9.0 | Zenoh 协议栈（需订阅功能） |
| xtensa-esp32s3-elf-gcc | — | 交叉编译工具链 |

### 网络

- 一个 2.4 GHz WiFi 热点
- 同一网络中的 Zenoh 发布者（如另一块 ESP32 上的 `z_pub.c`，或 `zenoh pub` 命令行）

---

## 代码详解

### 1. 许可证头部

```c
// Copyright (c) 2022 ZettaScale Technology
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
```

### 2. 头文件包含

```c
#include <esp_event.h>    /* ESP-IDF 事件循环 */
#include <esp_log.h>      /* ESP-IDF 日志 */
#include <esp_system.h>   /* ESP-IDF 系统 API */
#include <esp_wifi.h>     /* WiFi 驱动 */

#include <stdio.h>   /* printf */
#include <stdlib.h>  /* exit */
#include <string.h>  /* strcmp */

#include <nvs_flash.h>   /* NVS 闪存存储 */
#include <unistd.h>      /* sleep() */
#include <zenoh-pico.h>  /* Zenoh 客户端库 */

#include <freertos/FreeRTOS.h>     /* FreeRTOS 内核 */
#include <freertos/event_groups.h> /* 事件组 */
#include <freertos/task.h>         /* 任务 API */
```

### 3. 编译时守卫：`#if Z_FEATURE_SUBSCRIPTION == 1`

```c
#if Z_FEATURE_SUBSCRIPTION == 1
// ... 主程序体 ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
```

订阅功能可在编译 zenoh-pico 时关闭。`sdkconfig` 中确认：

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable subscription feature
```

### 4. WiFi 配置

```c
#define ESP_WIFI_SSID "SSID"
#define ESP_WIFI_PASS "PASS"
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

### 5. 全局状态

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

### 6. 会话模式选择

```c
#define CLIENT_OR_PEER 0
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#endif
```

### 7. 主题与轮询参数

```c
#define KEYEXPR "demo/example/**"

const size_t INTERVAL = 5000;   /* 轮询间隔（毫秒） */
const size_t SIZE     = 3;      /* 环形通道容量 */
```

| 常量 | 含义 |
|------|------|
| `KEYEXPR` | 主题过滤器——`**` 匹配任意子路径 |
| `INTERVAL` | 每轮检查新样本的时间间隔（5 秒） |
| `SIZE` | 环形缓冲区容量——最多缓存 3 个未读样本 |

**为什么设 `SIZE = 3`？** 实际应用中应根据以下因素调整：
- 预期发布频率
- 可容忍的最大延迟
- 可用 RAM（每个样本携带完整主题 + 负载）

环形满时新发布到达，**最旧样本会被静默丢弃**。这避免了反向压力到发布者。

### 8. WiFi 事件回调和 `wifi_init_sta()`

与其他示例相同的标准 ESP-IDF 模式：

```
WIFI_EVENT_STA_START        →  esp_wifi_connect()
WIFI_EVENT_STA_DISCONNECTED →  最多重试 5 次
IP_EVENT_STA_GOT_IP         →  设置事件组位，唤醒 app_main
```

`wifi_init_sta()` 阻塞直到 DHCP 成功。详细解析参见 `z_pub.md`。

### 9. `app_main()` — 入口函数

与 `z_sub.c`（回调订阅者）的差异在于**第五步**和**第六步**。

```
┌─────────────────────────────────────────────┐
│  第一步：NVS 初始化                         │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第二步：WiFi 连接（阻塞）                  │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第三步：构建 Zenoh 配置                    │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第四步：打开 Zenoh 会话                    │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第五步：声明订阅者（环形通道）             │  ← 关键差异
│  (z_ring_channel_sample_new + declare)      │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第六步：拉取循环                           │  ← 关键差异
│  (每 5 秒调用 z_try_recv)                   │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第七步：清理                               │
└─────────────────────────────────────────────┘
```

#### 第一步到第四步

与其他示例相同：NVS 初始化 → WiFi 连接 → 构建 Zenoh 配置 → 打开会话。

#### 第五步 — 声明环形通道订阅者

这是拉取订阅者与回调订阅者的核心区别。

```c
printf("Declaring Subscriber on '%s'...\n", KEYEXPR);

// 1. 创建环形通道对
z_owned_closure_sample_t       closure;
z_owned_ring_handler_sample_t  handler;
z_ring_channel_sample_new(&closure, &handler, SIZE);

// 2. 声明订阅者（传递闭包，而非回调函数）
z_owned_subscriber_t sub;
z_view_keyexpr_t     ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
if (z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(closure), NULL) < 0) {
    printf("Unable to declare subscriber.\n");
    exit(-1);
}
```

**`z_ring_channel_sample_new(&closure, &handler, SIZE)`** 创建有界 FIFO：

| 输出 | 类型 | 用途 |
|------|------|------|
| `closure` | `z_owned_closure_sample_t` | 传给 `z_declare_subscriber`；Zenoh 向其写入 |
| `handler` | `z_owned_ring_handler_sample_t` | 应用程序从此端读取 |
| `SIZE` | `size_t` | 最大缓冲样本数（3） |

**内部原理：** 闭包内部持有指向共享环形缓冲区的引用。发布到达时，Zenoh 的传输层调用闭包，将样本写入环形。处理器端可随时从环形中拉取。

**环形满时：** 最旧样本被覆盖。这是**有界、无锁**设计——慢速消费者不会阻塞发布者。

#### 第六步 — 拉取循环

```c
printf("Pulling data every %zu ms... Ring size: %zd\n", INTERVAL, SIZE);
z_owned_sample_t sample;
while (true) {
    z_result_t res;
    /*
     * 取出环形中所有待处理样本。
     * z_try_recv() 返回：
     *   Z_OK             → 成功取出一个样本
     *   Z_CHANNEL_NODATA → 环形为空（无等待的发布）
     */
    for (res = z_try_recv(z_loan(handler), &sample); res == Z_OK;
         res = z_try_recv(z_loan(handler), &sample)) {
        /* 打印主题和负载 */
        z_view_string_t keystr;
        z_keyexpr_as_view_string(z_sample_keyexpr(z_loan(sample)), &keystr);
        z_owned_string_t value;
        z_bytes_to_string(z_sample_payload(z_loan(sample)), &value);
        printf(">> [Subscriber] Pulled ('%.*s': '%.*s')\n",
               (int)z_string_len(z_loan(keystr)),
               z_string_data(z_loan(keystr)),
               (int)z_string_len(z_loan(value)),
               z_string_data(z_loan(value)));

        /* 释放拥有的资源 */
        z_drop(z_move(value));
        z_drop(z_move(sample));
    }

    if (res == Z_CHANNEL_NODATA) {
        /* 无数据——休眠后再试 */
        printf(">> [Subscriber] Nothing to pull... sleep for %zu ms\n", INTERVAL);
        z_sleep_ms(INTERVAL);
    } else {
        /* 意外错误——退出拉取循环 */
        break;
    }
}
```

**关键 API：`z_try_recv()`**

| 返回值 | 含义 |
|--------|------|
| `Z_OK` | 成功取出样本；处理之 |
| `Z_CHANNEL_NODATA` | 环形缓冲区为空——自上次取出后无新发布 |
| 其他 | 错误——应退出循环 |

**内部 `for` 循环**一次性取出所有样本：
```c
for (res = z_try_recv(...); res == Z_OK; res = z_try_recv(...))
```
这确保每个轮询周期完全清空环形。如果每次只调用一次 `z_try_recv()`，样本可能堆积并溢出缓冲区。

**每个样本的内存管理：**
1. 将负载转换为 owned 字符串：`z_bytes_to_string(...)`
2. 打印
3. 释放 owned 字符串：`z_drop(z_move(value))`
4. 释放样本本身：`z_drop(z_move(sample))`

跳过任意一个 `z_drop` 调用都会导致每个接收到的发布发生内存泄漏。

#### 第七步 — 清理

```c
z_drop(z_move(sub));      // 取消声明订阅者
z_drop(z_move(handler));  // 释放环形通道处理器
z_drop(z_move(s));        // 关闭会话
```

注意需要释放**订阅者和处理器**两者——它们是 `z_ring_channel_sample_new()` 创建的两个独立 owned 对象。

---

## 编译与烧录

### 1. 配置 WiFi

```c
#define ESP_WIFI_SSID "你的WiFi名称"
#define ESP_WIFI_PASS "你的WiFi密码"
```

### 2. 确认订阅功能已启用

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable subscription feature
```

### 3. 编译、烧录、监视

```bash
idf.py build flash monitor
```

预期输出（等待数据中）：

```
Connecting to WiFi...OK!
Opening Zenoh Session...OK
Declaring Subscriber on 'demo/example/**'...
Pulling data every 5000 ms... Ring size: 3
>> [Subscriber] Nothing to pull... sleep for 5000 ms
```

---

## 测试方法

### 使用 Python 批量发布器（推荐）

安装 zenoh-python 并运行本项目中的 `z_pull.py`：

```bash
pip install zenoh

# 发送 5 条消息，间隔 1 秒
uv run python3 scripts/z_pull.py

# 自定义：10 条消息，间隔 0.5 秒
uv run python3 scripts/z_pull.py -n 10 -d 0.5 "批量测试"

# Peer 模式
uv run python3 scripts/z_pull.py --mode peer
```

ESP32 在下个轮询周期输出：
```
>> [Subscriber] Pulled ('demo/example/test': 'Hello ESP32 pull!')
>> [Subscriber] Nothing to pull... sleep for 5000 ms
```

### 使用另一块 ESP32（z_pub.c）

烧录 `z_pub.c` 到另一块 ESP32 (S3 / C5)。它每秒发布一次。拉取订阅者每 5 秒窗口收集最多 3 个样本：

```
>> [Subscriber] Pulled ('demo/example/zenoh-pico-pub': '[   0] [ESPIDF]{ESP32} Publication...')
>> [Subscriber] Pulled ('demo/example/zenoh-pico-pub': '[   1] [ESPIDF]{ESP32} Publication...')
...
>> [Subscriber] Nothing to pull... sleep for 5000 ms
```

### 高速发布测试

测试环形缓冲区溢出，发布速度超过拉取速度：

```bash
# 50 条消息，间隔 0.1 秒 — 环形会溢出（容量 3）
uv run python3 scripts/z_pull.py -n 50 -d 0.1 "快速数据"
```

环形只保留最新的 **3 条** — 旧消息会被丢弃。增大 `SIZE` 可容纳更大缓冲区。

---

## 自定义指南

### 更改环形缓冲区大小

```c
const size_t SIZE = 64;   // 最多缓冲 64 个样本
```

更大的环形占用更多 RAM，但能容忍更长的轮询间隔。

### 更改轮询频率

```c
const size_t INTERVAL = 1000;   // 每秒轮询一次
```

### 非阻塞轮询（检查后继续其他工作）

```c
while (true) {
    z_result_t res;
    for (res = z_try_recv(z_loan(handler), &sample);
         res == Z_OK;
         res = z_try_recv(z_loan(handler), &sample)) {
        // 处理样本...
        z_drop(z_move(sample));
    }
    if (res == Z_CHANNEL_NODATA) {
        // 无数据——做其他工作而不是休眠
        process_sensor_readings();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

### 区分不同主题

```c
z_view_string_t keystr;
z_keyexpr_as_view_string(z_sample_keyexpr(z_loan(sample)), &keystr);
const char *key = z_string_data(z_view_string_loan(&keystr));

if (strstr(key, "temperature")) {
    handle_temperature(payload);
} else if (strstr(key, "humidity")) {
    handle_humidity(payload);
}
```

---

## 故障排除

### ❌ `Z_FEATURE_SUBSCRIPTION` 未定义

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable subscription feature
```

### ❌ `Unable to open session!`

| 原因 | 解决 |
|------|------|
| 无路由器（客户端模式） | 启动 `zenohd` 或切换到对等端模式 |
| WiFi 未连接 | 检查 SSID 和密码 |
| 防火墙 | 开放 UDP 7447 |

### ❌ `Unable to declare subscriber.`

通常是会话打开后的网络问题。检查路由器连接。

### ❌ 样本丢失（接收数据有间隔）

环形缓冲区溢出时丢弃最旧样本。增大 `SIZE` 或减小 `INTERVAL`：

```c
const size_t SIZE     = 64;     // 更大的缓冲区
const size_t INTERVAL = 1000;   // 更频繁轮询
```

### ❌ 环形通道函数链接失败

确保 zenoh-pico 编译时包含了订阅和环形通道支持。

---

## 对比：拉取 vs. 回调订阅者

| 决策因素 | 选拉取 (`z_pull.c`) | 选回调 (`z_sub.c`) |
|---------|--------------------|--------------------|
| 处理模型 | 轮询周期（每 N 毫秒检查） | 事件驱动（即时回调） |
| 可在处理器中阻塞？ | 不适用——在主循环中处理 | 可以——`sleep` 或 `vTaskDelay` 均可 |
| 最大延迟 | N 毫秒（轮询间隔） | 亚毫秒级 |
| CPU 占用 | 间歇性唤醒处理 | 无消息时空闲 |
| 溢出行为 | 丢弃最旧（有界缓冲区） | 不适用——回调是同步的 |
| RAM 占用 | 预分配环形缓冲区 | 极小（无缓冲） |

**选择拉取订阅者当：**
- 应用程序已有固定轮询周期（如传感器每秒读一次）
- 希望批量处理发布数据
- 能容忍轮询延迟

**选择回调订阅者当：**
- 需要对每条发布实时响应
- 发布频率低且希望无消息时零 CPU
- 不想管理缓冲区大小

---

## 参考资源

| 资源 | 链接 |
|------|------|
| Zenoh 发布/订阅概念 | https://zenoh.io/docs/manual/abstractions/#publish-subscribe |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF WiFi 驱动 | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html |
| FreeRTOS 事件组 | https://www.freertos.org/event-groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## 许可证

本文档对应的源代码采用 Eclipse Public License 2.0 或 Apache License 2.0 双许可——详见文件头部。
