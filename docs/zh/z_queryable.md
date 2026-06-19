# z_queryable.md — ESP32-S3 Zenoh 可查询节点（请求-响应）教程

[← 返回 docs](../README.md)

---

## 概述

`main/z_queryable.c` 是一个面向 **ESP32-S3** 的 Zenoh **可查询节点（Queryable）** 示例程序，运行于 **ESP-IDF v6.0** 框架之上。与 `z_get.c` 类似，它演示了 Zenoh 请求-响应模式的服务器端——ESP32 在一个主题上声明 Queryable，当其他节点发送 GET 查询时做出回复。

### 什么是 Queryable？

在 Zenoh 中，**Queryable** 相当于 REST API 端点：

```
客户端:  GET "demo/example/zenoh-pico-queryable?filter=all"
                │
                ▼
         Zenoh 路由器 (zenohd)
                │
                ▼
Queryable:  query_handler() 触发
                │
                ├─ 读取查询的主题、参数、负载
                ├─ 构建回复 "[ESPIDF]{ESP32} Queryable from Zenoh-Pico!"
                └─ 通过 z_query_reply() 发送回复
                │
                ▼
         客户端收到回复
```

与订阅者（接收推送数据）不同，Queryable **等待被询问**。

### 核心功能

| 功能 | 说明 |
|------|------|
| WiFi STA 连接 | 连接指定 SSID，自动重试（最多 5 次） |
| Zenoh 会话 | 以 Client 或 Peer 模式打开 Zenoh 会话 |
| Queryable 声明 | 为 `"demo/example/zenoh-pico-queryable"` 注册处理器 |
| 完整查询日志 | 打印主题表达式、参数和可选负载 |
| 回复发送 | 通过 `z_query_reply()` 响应静态值 |
| 内存安全 | 正确释放 owned 字符串，防止泄漏 |

---

## 前置条件

### 硬件

- ESP32-S3 开发板（如 ESP32-S3-DevKitC-1）
- USB-C 数据线

### 软件

| 工具 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | v6.0.1 | 嵌入式开发框架 |
| zenoh-pico | v1.9.0 | Zenoh 协议栈（需 queryable 支持） |
| xtensa-esp32s3-elf-gcc | — | 交叉编译工具链 |
| zenoh（命令行工具） | — | 用于发送测试 GET 查询 |

### 网络

- 一个 2.4 GHz WiFi 热点
- 同一网络中的 Zenoh 路由器（`zenohd`）或对等端

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

按类别分组：**ESP-IDF**、**C 标准库**、**第三方库**、**FreeRTOS**。

### 3. 编译时守卫

```c
#if Z_FEATURE_QUERYABLE == 1
// ... 主程序体 ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
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
#define LOCATOR ""            // 为空则通过 scout 发现路由器
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#endif
```

| 模式 | 连接方式 | 需要路由器？ |
|------|---------|-------------|
| 客户端 | 连接到 `zenohd` | 是 |
| 对等端 | UDP 多播直连 | 否 |

### 7. 主题和回复负载

```c
#define KEYEXPR "demo/example/zenoh-pico-queryable"
#define VALUE "[ESPIDF]{ESP32} Queryable from Zenoh-Pico!"
```

### 8. WiFi 事件回调

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

标准三事件模式：

| 事件 | 发生什么 |
|------|---------|
| `WIFI_EVENT_STA_START` | 调用 `esp_wifi_connect()` |
| `WIFI_EVENT_STA_DISCONNECTED` | 重试最多 `ESP_MAXIMUM_RETRY` 次 |
| `IP_EVENT_STA_GOT_IP` | 设置事件组位、重置重试计数 |

### 9. `wifi_init_sta()` — WiFi 初始化

与其他示例相同的标准 ESP-IDF 序列：

```
事件组 → 网络接口初始化 → 事件循环 → STA 接口 → WiFi 驱动初始化
→ 注册处理器 → 设置凭据 → 启动 → 阻塞（事件组）
→ 注销处理器 → 删除事件组
```

此函数**阻塞直到 DHCP 成功**（获得 IP 地址）。

### 10. `query_handler()` — 查询回调

这是每次远程节点发送针对我们主题的 GET 查询时运行的函数。

```c
void query_handler(z_loaned_query_t *query, void *ctx)
{
    (void)(ctx);  /* 未使用的上下文参数 */
```

#### 第一步：提取并打印主题表达式

```c
z_view_string_t keystr;
z_keyexpr_as_view_string(z_query_keyexpr(query), &keystr);
```

`z_query_keyexpr(query)` 返回 GET 查询所针对的主题的 loaned 引用。可能是精确主题或通配符匹配。

`z_view_string_t` 是**非拥有**类型——无需释放。

#### 第二步：提取并打印查询参数

```c
z_view_string_t params;
z_query_parameters(query, &params);
```

查询可携带 URL 风格参数，如 `?format=json&limit=100`。这些与主题分开提取。

#### 第三步：提取并打印查询负载

```c
z_owned_string_t payload_string;
z_bytes_to_string(z_query_payload(query), &payload_string);
if (z_string_len(z_loan(payload_string)) > 0) {
    printf("     with value '%.*s'\n", ...);
}
z_drop(z_move(payload_string));
```

有些 GET 查询携带负载（类似于 HTTP GET 的请求体）。我们：
1. 将负载字节转换为 owned 字符串
2. 若非空则打印
3. **释放之**——这对避免内存泄漏至关重要

#### 第四步：构建回复

```c
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);

z_owned_bytes_t reply_payload;
z_bytes_from_static_str(&reply_payload, VALUE);

z_query_reply(query, z_loan(ke), z_move(reply_payload), NULL);
```

**`z_query_reply()`** 向查询节点发送回复：

| 参数 | 说明 |
|------|------|
| `query` | 原始查询引用（loaned） |
| `z_loan(ke)` | 回复所用的主题 |
| `z_move(reply_payload)` | 负载（所有权已转移） |
| `NULL` | 无回复附件 |

**重要：** `z_query_reply()` 是**非阻塞的**。它将回复排队等待传输层投递。此外，一个 Queryable 可以对单个查询发送**多条回复**（用于聚合）。如需标记"最后一条"，可使用 `z_query_reply_final()`。

### 11. `app_main()` — 入口函数

```
┌─────────────────────────────────────────────┐
│  第一步：NVS 初始化                          │
│  → nvs_flash_init() + 擦除重试模式           │
├─────────────────────────────────────────────┤
│  第二步：WiFi 连接                           │
│  → wifi_init_sta() + 轮询防护                │
├─────────────────────────────────────────────┤
│  第三步：构建 Zenoh 配置                     │
│  → z_config_default() + zp_config_insert()   │
├─────────────────────────────────────────────┤
│  第四步：打开 Zenoh 会话                     │
│  → z_open() — 连接到路由器或对等端           │
├─────────────────────────────────────────────┤
│  第五步：声明 Queryable                      │
│  → z_declare_queryable() — 处理器上线        │
├─────────────────────────────────────────────┤
│  第六步：空闲循环                             │
│  → while(1) { sleep(1); }                    │
├─────────────────────────────────────────────┤
│  第七步：清理（不可达）                       │
│  → z_drop(qable) → z_drop(session)           │
└─────────────────────────────────────────────┘
```

#### 第一步到第四步

标准 ESP-IDF + Zenoh 初始化。详细解析参见 `z_get.md`。

#### 第五步 — 声明 Queryable

```c
printf("Declaring Queryable on %s...", KEYEXPR);

z_owned_closure_query_t callback;
z_closure(&callback, query_handler, NULL, NULL);

z_owned_queryable_t qable;
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);

if (z_declare_queryable(z_loan(s), &qable, z_loan(ke), z_move(callback), NULL) < 0) {
    printf("Unable to declare queryable.\n");
    exit(-1);
}
```

**`z_closure(&callback, query_handler, NULL, NULL)`** 构建 Zenoh 闭包：
1. `query_handler` — 每次查询到达时调用的函数
2. `NULL` — 无上下文指针
3. `NULL` — 无 drop 函数

**`z_declare_queryable(...)`** 向 Zenoh 会话注册 Queryable：
- `z_loan(s)` — 借用的会话引用
- `&qable` — 接收 Queryable 句柄
- `z_loan(ke)` — 借用的主题
- `z_move(callback)` — 闭包所有权转移给会话
- `NULL` — 无附加选项

此调用成功后，Queryable 即**生效**。任何匹配的 GET 查询都将调用 `query_handler`。

#### 第六步 — 空闲循环

```c
while (1) {
    sleep(1);
}
```

Queryable 在后台运行。我们只需保持任务存活。

#### 第七步 — 清理（不可达）

```c
z_drop(z_move(qable));   // 取消声明 Queryable
z_drop(z_move(s));       // 关闭会话
```

---

## 编译与烧录

### 1. 配置 WiFi

```c
#define ESP_WIFI_SSID "你的WiFi名称"
#define ESP_WIFI_PASS "你的WiFi密码"
```

### 2. 确认 Queryable 已启用

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable queryable feature
```

### 3. 编译、烧录、监视

```bash
idf.py build flash monitor
```

预期输出：

```
Connecting to WiFi...OK!
Opening Zenoh Session...OK
Declaring Queryable on demo/example/zenoh-pico-queryable...OK
Zenoh setup finished!
```

---

## 测试方法

### 使用 Python 查询器（推荐）

安装 zenoh-python 并运行本项目中的 `z_get.py`：

```bash
pip install zenoh

# 查询默认 key expression
uv run python3 scripts/z_get.py

# 携带负载查询
uv run python3 scripts/z_get.py "Hello ESP32!"

# Peer 模式，自定义超时
uv run python3 scripts/z_get.py --mode peer --timeout 5.0
```

ESP32 输出：
```
 >> [Queryable handler] Received Query 'demo/example/zenoh-pico-queryable'
```

PC 输出：
```
>> Received ('demo/example/zenoh-pico-queryable': '[ESPIDF]{ESP32} Queryable from Zenoh-Pico!')
```

---

## 自定义指南

### 根据查询负载动态回复

```c
void query_handler(z_loaned_query_t *query, void *ctx) {
    z_owned_string_t payload;
    z_bytes_to_string(z_query_payload(query), &payload);

    z_owned_bytes_t reply;
    if (z_string_len(z_loan(payload)) > 0 &&
        strcmp(z_string_data(z_loan(payload)), "time") == 0) {
        char timebuf[64];
        sprintf(timebuf, "Current uptime: %d secs", uptime_seconds);
        z_bytes_from_static_str(&reply, timebuf);
    } else {
        z_bytes_from_static_str(&reply, VALUE);
    }

    z_query_reply(query, z_loan(ke), z_move(reply), NULL);
    z_drop(z_move(payload));
}
```

### 多个 Queryable

声明不同主题的处理器：

```c
z_declare_queryable(..., "sensor/temperature", ...);
z_declare_queryable(..., "sensor/humidity", ...);
```

### 使用上下文指针

传递结构体在处理器之间共享状态：

```c
typedef struct { int counter; } query_ctx_t;

query_ctx_t *ctx = malloc(sizeof(query_ctx_t));
ctx->counter = 0;
z_closure(&callback, query_handler, ctx, free_context);

void query_handler(z_loaned_query_t *query, void *ctx_ptr) {
    query_ctx_t *ctx = (query_ctx_t *)ctx_ptr;
    ctx->counter++;
    // ... 回复中包含计数器值
}
```

---

## 故障排除

### ❌ `Unable to open session!`

| 原因 | 解决 |
|------|------|
| 未运行路由器（客户端模式） | 启动 `zenohd` |
| WiFi 未连接 | 验证 SSID/密码 |
| 不同子网 | 必须在同一子网才能 scout 发现 |
| 防火墙 | 开放 UDP 7447（scout）和 TCP/UDP 7447（会话） |

### ❌ `Unable to declare queryable.`

会话可能已断开。检查网络稳定性。

### ❌ GET 返回无数据

| 检查项 | 验证内容 |
|--------|---------|
| 主题匹配 | 两端都使用 `"demo/example/zenoh-pico-queryable"` |
| 会话模式 | Client ↔ Client（通过路由器）或 Peer ↔ Peer（直连） |
| 路由器日志 | `zenohd` 显示两端都已连接？ |

### ❌ `Z_FEATURE_QUERYABLE` 未定义

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable queryable feature
```

---

## 参考资源

| 资源 | 链接 |
|------|------|
| Zenoh Queryable 概念 | https://zenoh.io/docs/manual/abstractions/#queryable |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| Zenoh CLI get 命令 | https://zenoh.io/docs/getting-started/quick-test/#rest-api |
| ESP-IDF 编程指南 | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/ |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## 许可证

本文档对应的源代码采用 Eclipse Public License 2.0 或 Apache License 2.0 双许可——详见文件头部。
