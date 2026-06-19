# z_get.md — ESP32-S3 Zenoh 可查询节点（请求-响应）教程

[← 返回 docs](../README.md)

---

## 概述

`main/z_get.c` 是一个面向 **ESP32-S3** 的 Zenoh **可查询节点（Queryable）** 示例程序。它演示了 Zenoh 请求-响应模式的服务器端——ESP32 在一个主题上声明 Queryable，当其他节点发送 GET 查询时做出回复。

### 什么是 Queryable？

在 Zenoh 中，**Queryable** 类似于 HTTP 服务器端点：

```
[客户端]  ─── GET "demo/example/zenoh-pico-queryable" ───→  [Queryable (ESP32)]
               └─ Payload: "query data?"                      │
                                                              │
               ←── 回复: "[ESPIDF]{ESP32} Queryable..." ──────┘
```

与订阅者（接收推送数据）不同，Queryable **等待被询问**。请求节点发送 GET 查询，Queryable 的处理函数运行并产生回复。

### 核心功能

| 功能 | 说明 |
|------|------|
| WiFi STA 连接 | 连接指定 SSID，自动重试（最多 5 次） |
| Zenoh 会话 | 以 Client 或 Peer 模式打开 Zenoh 会话 |
| Queryable 声明 | 为 `"demo/example/zenoh-pico-queryable"` 注册处理器 |
| 请求-响应 | 记录传入查询的主题、参数和负载 |
| 内存安全 | 正确释放在堆上分配的字符串，防止内存泄漏 |
| 错误处理 | Zenoh 操作失败立即退出并打印清晰信息 |

### 数据流

```
[Zenoh 客户端]                           [ESP32-S3 Queryable]
      │                                         │
      │  GET "demo/example/zenoh-pico-queryable"
      │  ──────────────────────────────────────→│
      │                                         │ query_handler() 触发：
      │                                         │  1. 打印查询详细信息
      │                                         │  2. 构建回复负载
      │                                         │  3. 调用 z_query_reply()
      │  ←──────────────────────────────────────│
      │  回复: [ESPIDF]{ESP32} Queryable...     │
      ▼                                        ▼
```

---

## 前置条件

### 硬件

- ESP32-S3 开发板（如 ESP32-S3-DevKitC-1）
- USB-C 数据线

### 软件

| 工具 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | v6.0.1 | 嵌入式开发框架 |
| zenoh-pico | v1.9.0 | Zenoh 协议栈（必须启用 queryable） |
| xtensa-esp32s3-elf-gcc | — | 交叉编译工具链 |
| zenoh（CLI 或 Python） | — | 从 PC 发送测试查询 |

### 网络

- 一个 2.4 GHz WiFi 热点（SSID + 密码）
- 一个 Zenoh 路由器（`zenohd`）**或**对等端在同一网络中（用于路由 GET 查询）

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

#include <stdio.h>   /* printf / fprintf */
#include <stdlib.h>  /* malloc / free / exit */
#include <string.h>  /* strcmp, memset */

#include <nvs_flash.h>   /* NVS 闪存存储 */
#include <unistd.h>      /* sleep() */
#include <zenoh-pico.h>  /* Zenoh 客户端库 */

#include <freertos/FreeRTOS.h>     /* FreeRTOS 内核 */
#include <freertos/event_groups.h> /* 事件组 */
#include <freertos/task.h>         /* 任务 API */
```

按 ESP-IDF 包含分类规则分为四组：**ESP-IDF**、**C 标准库**、**第三方库**、**FreeRTOS**。

### 3. 编译时守卫：`#if Z_FEATURE_QUERYABLE == 1`

```c
#if Z_FEATURE_QUERYABLE == 1
// ... 主程序体 ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
```

Queryable 功能可在编译 zenoh-pico 时关闭。**如果遇到构建错误**，请检查：

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable queryable feature
```

### 4. WiFi 配置宏

```c
#define ESP_WIFI_SSID "SSID"
#define ESP_WIFI_PASS "PASS"
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

| 宏 | 含义 |
|----|------|
| `ESP_WIFI_SSID` | WiFi 热点名称（仅 2.4 GHz） |
| `ESP_WIFI_PASS` | WiFi 密码 |
| `ESP_MAXIMUM_RETRY` | 断开后最大重连次数 |
| `WIFI_CONNECTED_BIT` | 事件组比特位（`BIT0 = 0x01`）表示 WiFi 就绪 |

### 5. 全局状态变量

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

| 变量 | 用途 |
|------|------|
| `s_is_wifi_connected` | `app_main` 轮询此标志；`GOT_IP` 时设为 `true` |
| `s_event_group_handler` | FreeRTOS 事件组，用于阻塞等待 WiFi 连接 |
| `s_retry_count` | 重连次数计数器；达到 `ESP_MAXIMUM_RETRY` 后停止 |

### 6. Zenoh 模式选择

```c
#define CLIENT_OR_PEER 0   // 0: 客户端模式; 1: 对等端模式
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""         // 为空则通过 scout 自动发现
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#endif
```

| 模式 | `CLIENT_OR_PEER` | 连接方式 | 优点 | 缺点 |
|------|-----------------|---------|------|------|
| **客户端** | 0 | 连接到 Zenoh 路由器（`zenohd`） | 可靠、可扩展 | 需要路由器 |
| **对等端** | 1 | UDP 多播直连，无需路由器 | 简单、自包含 | 可扩展性较差、多播有局限 |

**建议**：从**客户端模式**开始。先在 PC 上运行 `zenohd`，再运行 ESP32。

### 7. 主题和回复值

```c
#define KEYEXPR "demo/example/zenoh-pico-queryable"
#define VALUE "[ESPIDF]{ESP32} Queryable from Zenoh-Pico!"
```

| 宏 | 用途 |
|----|------|
| `KEYEXPR` | 此 Queryable 应答的主题——匹配此主题的查询会触发处理器 |
| `VALUE` | 发送回每个查询的静态回复负载 |

### 8. WiFi 事件回调

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

三个事件驱动 WiFi 生命周期：

```
WIFI_EVENT_STA_START         →  esp_wifi_connect()
WIFI_EVENT_STA_DISCONNECTED  →  未超限则重试
IP_EVENT_STA_GOT_IP          →  设置事件组位、重置重试计数
```

回调运行在 ESP-IDF 的事件任务上下文中——保持简短且不阻塞。

### 9. `wifi_init_sta()` — WiFi 初始化（阻塞）

阻塞调用任务直到 ESP32 获得有效 IP 地址。标准的 ESP-IDF 序列：

```
事件组    →  xEventGroupCreate()
网络栈    →  esp_netif_init() + esp_event_loop_create_default()
STA 接口  →  esp_netif_create_default_wifi_sta()
WiFi 驱动 →  esp_wifi_init(&config)
处理器    →  注册 event_handler 处理 WIFI_EVENT + IP_EVENT
配置      →  设置 SSID/密码、模式 = STA、启动
阻塞      →  xEventGroupWaitBits(... portMAX_DELAY)
清理      →  注销处理器、删除事件组
```

### 10. `query_handler()` — 查询回调

这是 Queryable 的核心函数。每次其他节点发送 GET 查询时运行。

#### 第一步：提取并打印主题表达式

```c
z_view_string_t keystr;
z_keyexpr_as_view_string(z_query_keyexpr(query), &keystr);
```

`z_view_string_t` 是**借用的**字符串——无需释放。它借用自查询的内部数据。

#### 第二步：提取并打印查询参数

```c
z_view_string_t params;
z_query_parameters(query, &params);
```

查询可以携带 URL 风格参数，如 `?threshold=0.5&unit=celsius`。这些参数与主题表达式分开提取。

#### 第三步：提取并打印查询负载（如有）

```c
z_owned_string_t payload_string;
z_bytes_to_string(z_query_payload(query), &payload_string);
if (z_string_len(z_loan(payload_string)) > 0) {
    printf("     with value '%.*s'\n", ...);
}
z_drop(z_move(payload_string));  // 必须释放此拥有的字符串！
```

查询可能带有负载（请求的"主体"）。我们将其转换为拥有的字符串、打印，然后**释放**以避免内存泄漏。

#### 第四步：构建并发送回复

```c
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);

z_owned_bytes_t reply_payload;
z_bytes_from_static_str(&reply_payload, VALUE);

z_query_reply(query, z_loan(ke), z_move(reply_payload), NULL);
```

通过 `z_query_reply()` 发送回复：
1. 复用查询所针对的相同主题表达式
2. 将静态 `VALUE` 字符串打包为 Zenoh `Bytes` 负载
3. 发送回复（非阻塞——排队等待传输层投递）

**重要：** `z_query_reply()` **不会结束**查询。一个 Queryable 可以针对单个查询发送多条回复（用于聚合场景）。如果需要标记"这是最后一条回复"，可使用 `z_query_reply_final()`（如 API 版本支持）。

### 11. `app_main()` — 入口函数

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
│  (z_config_default + zp_config_insert)      │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第四步：打开 Zenoh 会话                    │
│  (z_open) — 连接到路由器或对等端            │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第五步：声明 Queryable                     │
│  (z_declare_queryable)                      │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第六步：空闲循环                           │
│  (while(1) { sleep(1); })                   │
│  → query_handler 在每个 GET 时运行          │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第七步：清理（不可达）                     │
└─────────────────────────────────────────────┘
```

#### 第一步 — NVS 初始化

```c
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);
```

带有分区恢复的标准 ESP-IDF NVS 初始化。擦除重试模式处理：
- `ESP_ERR_NVS_NO_FREE_PAGES` — 分区已满
- `ESP_ERR_NVS_NEW_VERSION_FOUND` — IDF 升级后 NVS 格式变更

#### 第二步 — WiFi 连接

```c
printf("Connecting to WiFi...");
wifi_init_sta();
while (!s_is_wifi_connected) {
    printf(".");
    sleep(1);
}
printf("OK!\n");
```

`wifi_init_sta()` 内部通过事件组阻塞；轮询循环是二次安全防护。

#### 第三步 — 构建 Zenoh 配置

```c
z_owned_config_t config;
z_config_default(&config);
zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, MODE);
if (strcmp(LOCATOR, "") != 0) {
    if (strcmp(MODE, "client") == 0)
        zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, LOCATOR);
    else
        zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, LOCATOR);
}
```

- `z_config_default()` — 以 Zenoh 默认参数开始
- `zp_config_insert()` — 覆盖单个配置键
- `z_loan_mut()` — 为调用借用可变的 config 引用
- 空 `LOCATOR`（客户端模式）= 通过 scout 自动发现路由器

#### 第四步 — 打开 Zenoh 会话

```c
printf("Opening Zenoh Session...");
z_owned_session_t s;
if (z_open(&s, z_move(config), NULL) < 0) {
    printf("Unable to open session!\n");
    exit(-1);
}
printf("OK\n");
```

`z_open()` 建立传输连接并协商能力。成功后，`config` 已被**移动**（失效）——不要再次使用。

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

**闭包构建：**
```c
z_closure(&callback, query_handler, NULL, NULL);
```
- 参数 1：闭包输出
- 参数 2：处理函数（每个查询调用）
- 参数 3：可选的上下文指针（`NULL`）
- 参数 4：可选的 drop 函数（`NULL`）

**声明后，Queryable 即生效。** 任何针对 `"demo/example/zenoh-pico-queryable"` 的 GET 查询都会触发 `query_handler`。

#### 第六步 — 空闲循环

```c
while (1) {
    sleep(1);
}
```

Queryable 在后台异步运行。我们只需保持任务存活。

#### 第七步 — 清理（不可达）

```c
z_drop(z_move(qable));   // 取消声明 Queryable
z_drop(z_move(s));       // 关闭会话
```

展示正确的销毁顺序。

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

### 使用 Zenoh CLI

在相同网络的 PC 上发送查询：

```bash
zenoh get -k "demo/example/zenoh-pico-queryable"
```

ESP32 输出：
```
 >> [Queryable handler] Received Query 'demo/example/zenoh-pico-queryable'
```

CLI 输出：
```
>> Received ('demo/example/zenoh-pico-queryable': '[ESPIDF]{ESP32} Queryable from Zenoh-Pico!')
```

### 携带负载的查询

```bash
zenoh get -k "demo/example/zenoh-pico-queryable" -v "hello"
```

ESP32 输出：
```
 >> [Queryable handler] Received Query 'demo/example/zenoh-pico-queryable'
     with value 'hello'
```

### 带参数的查询

```bash
zenoh get -k "demo/example/zenoh-pico-queryable?format=json&limit=10"
```

---

## 自定义指南

### 更改回复值

```c
#define VALUE "{\"sensor\": \"temperature\", \"value\": 25.3}"
```

### 添加查询路由逻辑

```c
void query_handler(z_loaned_query_t *query, void *ctx) {
    z_view_string_t params;
    z_query_parameters(query, &params);
    // 如果参数包含 "format=json"，回复 JSON；否则回复纯文本
}
```

### 注册多个 Queryable

```c
z_declare_queryable(..., "sensor/temperature", ...);
z_declare_queryable(..., "sensor/humidity", ...);
```

### 使用上下文指针

```c
typedef struct { int counter; } query_ctx_t;
query_ctx_t *ctx = malloc(sizeof(query_ctx_t));
z_closure(&callback, query_handler, ctx, free_context);
```

---

## 故障排除

### ❌ `Unable to open session!`

| 可能原因 | 解决方法 |
|---------|----------|
| 未运行 `zenohd`（客户端模式） | 在同一网络的 PC 上启动 `zenohd` |
| WiFi 连接失败 | 检查 SSID 和密码 |
| 防火墙阻止 Zenoh 端口 | 开放 TCP/UDP 7447 |
| 不同子网 | Scout 发现必须在同一子网中 |

### ❌ `Unable to declare queryable.`

通常在打开会话后立即发生。检查：
- 网络连接（ESP32 是否仍连接 WiFi？）
- Zenoh 路由器是否仍在运行

### ❌ 发送 GET 后无响应

| 可能原因 | 解决方法 |
|---------|----------|
| 主题不匹配 | GET 和 Queryable 必须使用相同的 `KEYEXPR` |
| 会话模式不匹配 | 混合客户端/对等端模式但无路由器桥接 |
| 路由器未转发 | 检查 `zenohd` 日志——确认两端都已连接 |

---

## 参考资源

| 资源 | 链接 |
|------|------|
| Zenoh Queryable 概念 | https://zenoh.io/docs/manual/abstractions/#queryable |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF WiFi 驱动 | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html |
| FreeRTOS 事件组 | https://www.freertos.org/event-groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## 许可证

本文档对应的源代码采用 Eclipse Public License 2.0 或 Apache License 2.0 双许可——详见文件头部。
