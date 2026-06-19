# z_scout.md — ESP32-S3 Zenoh 网络发现（Scout）教程

[← 返回 docs](../README.md)

---

## 概述

`main/z_scout.c` 是一个面向 **ESP32-S3** 的 Zenoh **网络发现（Scout）** 示例程序，运行于 **ESP-IDF v6.0** 框架之上。它通过发送 UDP 多播查询来发现局域网中的所有 Zenoh 节点（路由器、对等端、客户端），并以格式化方式打印每个节点的 `Hello` 回复。

### 什么是 Scouting？

Zenoh 的 Scouting 相当于在网络上喊 **"有人用 Zenoh 吗？"** 然后收听回复。每个 Zenoh 节点回复一条 **Hello** 消息，包含：

| Hello 字段 | 说明 | 示例 |
|-----------|------|------|
| **ZID** | Zenoh ID — 128 位全局唯一标识符 | `Some(A1B2C3D4E5F6...)` |
| **WhatAmI** | 节点角色 | `"router"`、`"peer"` 或 `"client"` |
| **Locators** | 通信端点地址 | `["tcp/192.168.1.100:7447"]` |

### Scout vs. Ping

| 对比维度 | Ping | Zenoh Scout |
|---------|------|-------------|
| 检测目标 | IP 可达性 | Zenoh 协议可用性 |
| 回复内容 | ICMP echo reply | Hello（含 ZID、角色、地址） |
| 需要目标地址？ | 是 | 否（多播） |
| 用途 | 网络连通性 | Zenoh 网络拓扑发现 |

### 核心功能

| 功能 | 说明 |
|------|------|
| WiFi STA 连接 | 连接指定 SSID，自动重试（最多 5 次） |
| Zenoh Scout | 发送 UDP 多播查询，收集 Hello 回复 |
| 格式化打印 | 自定义打印函数处理 ZID、WhatAmI、地址列表 |
| 异步回调 | Scout 后台运行，结果通过回调返回 |
| 计数器跟踪 | 记录收到多少个 Hello 回复 |

### 数据流

```
[ESP32-S3] --- WiFi STA ---> [WiFi AP]
    │
    │  "有人在用 Zenoh 吗？"  (UDP 多播 scout)
    │
    ├──<── Hello {zid: A1B2..., whatami: "router", locators: ["tcp/..."]}
    ├──<── Hello {zid: C3D4..., whatami: "peer",   locators: ["udp/..."]}
    │
    ▼
串口控制台打印每个 Hello 的详细信息
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
| zenoh-pico | v1.9.0 | Zenoh 协议栈（必须启用 scouting） |
| xtensa-esp32s3-elf-gcc | — | 交叉编译工具链 |

### 网络

- 一个 2.4 GHz WiFi 热点（SSID + 密码）
- 至少一个 Zenoh 节点（如运行中的 `zenohd` 或其他 Zenoh 设备）在**同一网段**——Scout 使用 UDP 多播，通常不跨子网

---

## 代码详解

以下按源代码出现顺序逐一讲解各部分。

### 1. 许可证头部

```c
// Copyright (c) 2022 ZettaScale Technology
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
```

双许可证：**Eclipse Public License 2.0** 或 **Apache License 2.0**。

文件顶部的注释块说明了程序目的、流程以及 Scout 与 Ping 的区别——建议先阅读这段来了解整体思路。

### 2. 头文件包含

```c
#include <esp_event.h>    /* ESP-IDF 事件循环 */
#include <esp_log.h>      /* ESP-IDF 日志 */
#include <esp_system.h>   /* ESP-IDF 系统 API */
#include <esp_wifi.h>     /* WiFi 驱动 */

#include <stdio.h>   /* printf / fprintf */
#include <stdlib.h>  /* malloc / free */
#include <string.h>  /* 字符串操作 */

#include <nvs_flash.h>   /* NVS 闪存存储 */
#include <unistd.h>      /* sleep() */
#include <zenoh-pico.h>  /* Zenoh 客户端库 */

#include <freertos/FreeRTOS.h>     /* FreeRTOS 内核 */
#include <freertos/event_groups.h> /* 事件组 */
#include <freertos/task.h>         /* 任务 API */
```

按 `.clang-format` 的 `IncludeCategories` 规则分为四组：**ESP-IDF**、**C 标准库**、**第三方库**、**FreeRTOS**。

### 3. 编译时特性守卫：`#if Z_FEATURE_SCOUTING == 1`

```c
#if Z_FEATURE_SCOUTING == 1
// ... 主程序体 ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
```

Scout 功能可在编译 zenoh-pico 时关闭以节省固件空间。此守卫确保关闭时打印清晰的错误信息而非链接失败。

**如果编译失败，请检查：**

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable scouting feature
```

### 4. WiFi 配置宏

```c
#define ESP_WIFI_SSID "Your_SSID"    // ← 替换为你的 WiFi SSID
#define ESP_WIFI_PASS "Your_Password" // ← 替换为你的 WiFi 密码
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

| 宏 | 含义 |
|----|------|
| `ESP_WIFI_SSID` | WiFi 热点名称（2.4 GHz，ESP32-S3 不支持 5 GHz） |
| `ESP_WIFI_PASS` | WiFi 密码 |
| `ESP_MAXIMUM_RETRY` | 断开后最大重连次数 |
| `WIFI_CONNECTED_BIT` | 事件组比特位（`BIT0 = 0x01`），表示 DHCP 已分配 IP |

> ⚠️ **安全提示**：此处为开发环境示例凭据。**切勿将真实密码提交到版本控制**。生产环境应使用 NVS 或 ESP-IDF 的 `wifi_provisioning` 组件。

### 5. 全局状态变量

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

文件作用域的静态变量，在 WiFi 事件回调和 `app_main` 之间共享：

| 变量 | 用途 |
|------|------|
| `s_is_wifi_connected` | `app_main` 轮询此标志；DHCP 成功后设为 `true` |
| `s_event_group_handler` | FreeRTOS 事件组句柄——用于唤醒阻塞等待的主任务 |
| `s_retry_count` | 记录连续重连次数；达到 `ESP_MAXIMUM_RETRY` 后停止 |

### 6. WiFi 事件回调

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

ESP-IDF 的事件系统在 WiFi 状态变化时调用此回调。三个关键事件：

| 事件 | 触发条件 | 处理逻辑 |
|------|---------|----------|
| `WIFI_EVENT_STA_START` | WiFi 驱动初始化完成 | 调用 `esp_wifi_connect()` 开始连接 |
| `WIFI_EVENT_STA_DISCONNECTED` | 与 AP 断开连接 | 若重试次数未达上限则重连，递增计数器 |
| `IP_EVENT_STA_GOT_IP` | DHCP 分配 IP 成功 | 设置事件组位唤醒 `app_main`，重置重试计数 |

**为什么要等 `GOT_IP` 而不是 `CONNECTED`？** `CONNECTED` 事件在 WiFi 关联完成时触发（二层），但此时还没有 IP 地址。`GOT_IP` 只在 DHCP 成功后才触发（三层），此时才能使用 TCP/UDP。

### 7. `wifi_init_sta()` — WiFi 初始化（阻塞）

此函数会阻塞调用任务，直到 ESP32 获得有效 IP 地址。

```
xEventGroupCreate()           ← 创建事件组
       │
esp_netif_init()              ← 初始化 TCP/IP 栈
esp_event_loop_create_default()
esp_netif_create_default_wifi_sta()
       │
esp_wifi_init(&config)        ← 初始化 WiFi 驱动
       │
注册 event_handler             ← 注册 WIFI_EVENT + IP_EVENT 处理器
       │
esp_wifi_set_mode(WIFI_MODE_STA)
esp_wifi_set_config(...)
esp_wifi_start()              ← 启动 WiFi
       │
xEventGroupWaitBits(...)      ← 阻塞等待 GOT_IP
       │
注销处理器 + 删除事件组         ← 清理
       │
s_is_wifi_connected = true
```

关键细节：

```c
// 必须先在启动 WiFi 前注册处理器，否则可能错过 STA_START 事件
esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...);
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...);

// C99 指定初始化器填充 WiFi 配置结构体
wifi_config_t wifi_config = {
    .sta = {
        .ssid = ESP_WIFI_SSID,
        .password = ESP_WIFI_PASS,
    }
};

// 阻塞等待——阻塞期间 CPU 占用为 0
xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT,
                    pdFALSE, pdFALSE, portMAX_DELAY);
```

### 8. `fprintzid()` — Zenoh ID 格式化打印

```c
void fprintzid(FILE *stream, z_id_t zid)
```

`z_id_t` 是 128 位全局唯一标识符，以原始字节数组存储。此函数将其格式化为大写十六进制字符串。

```
输入:  {0xA1, 0xB2, 0xC3, 0xD4, ...}
输出: Some(A1B2C3D4E5F6...)

输入:  ZID 长度为 0
输出: None
```

使用 `%02X` 逐字节打印（大写、补零）。

### 9. `fprintwhatami()` — 节点角色格式化打印

```c
void fprintwhatami(FILE *stream, z_whatami_t whatami)
```

将 `z_whatami_t` 枚举转换为可读字符串：

```c
z_whatami_to_view_string(whatami, &s);
fprintf(stream, "\"%.*s\"", ...);
```

| 枚举值 | 打印输出 |
|--------|---------|
| `Z_WHATAMI_ROUTER` | `"router"` |
| `Z_WHATAMI_PEER` | `"peer"` |
| `Z_WHATAMI_CLIENT` | `"client"` |

`%.*s` 格式接受 `(长度, 指针)` 对——即使内部字符串未空字符结尾也是安全的。

### 10. `fprintlocators()` — 地址列表格式化打印

```c
void fprintlocators(FILE *stream, const z_loaned_string_array_t *locs)
```

Locator 是 Zenoh 节点的通信端点地址。此函数以 JSON 风格数组打印：

```
输入:  ["tcp/192.168.1.100:7447", "udp/192.168.1.100:7447"]
输出: ["tcp/192.168.1.100:7447", "udp/192.168.1.100:7447"]
```

逗号分隔逻辑避免末尾多余逗号：
```c
if (i < z_string_array_len(locs) - 1)
    fprintf(stream, ", ");
```

### 11. `fprinthello()` — Hello 消息完整打印

```c
void fprinthello(FILE *stream, const z_loaned_hello_t *hello)
```

组合上述三个打印函数，输出格式化结果：

```
Hello { zid: Some(A1B2C3D4E5F6), whatami: "router", locators: ["tcp/192.168.1.100:7447"] }
```

此函数由 Scout 回调在每次收到 Hello 时调用。

### 12. `callback()` — Scout 回复回调

```c
void callback(z_loaned_hello_t *hello, void *context)
```

Zenoh 每次收到节点回复时调用此函数：

```c
fprinthello(stdout, hello);    // 向串口打印 Hello
fprintf(stdout, "\n");
(*(int *)context)++;            // 回复计数器加一
```

`context` 指针指向 `app_main` 中分配的 `int *` 计数器。

### 13. `drop()` — Scout 结束回调

```c
void drop(void *context)
```

Scout 操作完成（超时或手动取消）时调用：

```c
int count = *(int *)context;
free(context);
if (!count)
    printf("Did not find any zenoh process.\n");
else
    printf("Dropping scout results.\n");
```

在此完成清理：
- **释放** `app_main` 中分配的计数器内存
- **打印**摘要——找到 N 个节点或未找到任何节点
- `if (!count)` 检查对调试特别有用：如果网络中没有 Zenoh 节点，你会得到明确提示而非静默失败

### 14. `app_main()` — 入口函数

应用程序按三步顺序执行：

```
┌─────────────────────────────────────────────┐
│  第一步：NVS 初始化                         │
│  (nvs_flash_init)                           │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第二步：WiFi 连接（阻塞）                  │
│  (wifi_init_sta)                            │
│  → 阻塞直到 DHCP 分配 IP                    │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  第三步：Zenoh Scout                        │
│  1. 分配上下文计数器                        │
│  2. 创建默认 Zenoh 配置                     │
│  3. 创建闭包（callback + drop）             │
│  4. 调用 z_scout() — 发送 UDP 多播          │
│  → 异步打印 Hello 回复                      │
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

NVS（非易失性存储）用于存储 WiFi 校准数据和 DHCP 客户端配置。擦除重试模式处理固件升级后的分区格式变更。

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

轮询循环是**二次安全防护**——即使事件组机制失效，执行也不会在无 WiFi 时继续。

#### 第三步 — Zenoh Scout

```c
int *context = (int *)malloc(sizeof(int));
*context = 0;

z_owned_config_t config;
z_config_default(&config);

z_owned_closure_hello_t closure;
z_closure_hello(&closure, callback, drop, context);

printf("Scouting...\n");
z_scout(z_config_move(&config), z_closure_hello_move(&closure), NULL);
```

这是核心 Scout 操作：

1. **上下文分配**——堆上分配的计数器，记录收到多少 Hello 回复
2. **默认配置**——Scout 使用默认参数（UDP 多播端口 7446）
3. **闭包构建**——绑定 `callback`（每次回复）和 `drop`（完成时）与上下文
4. **`z_scout()`**——发送 UDP 多播查询并立即返回

**异步行为：** `z_scout()` **不阻塞**。它发送查询、注册回调、立即返回。Zenoh 的传输层在后台收集回复。收集超时（默认约 1 秒）时触发 `drop` 函数。

**第三个参数（`NULL`）：** 可选的 Scout 配置——传递 `NULL` 使用默认值。可以通过配置控制 Scout 范围（如同子网或跨子网）。

**重要提示：** `z_scout()` 返回后，`app_main` 也会立即返回。在这个简单示例中，程序可能在所有 Scout 回复到达之前就退出了。生产环境中应使用信号量或更长的延迟保持任务存活直到 `drop` 被调用。

---

## 编译与烧录

### 1. 配置 WiFi 凭据

将 `main/z_scout.c` 中的 WiFi 凭据替换为你自己的网络信息：

```c
#define ESP_WIFI_SSID "你的WiFi名称"  // ← 你的 2.4 GHz WiFi 名称
#define ESP_WIFI_PASS "你的WiFi密码"  // ← 你的 WiFi 密码
```

### 2. 确认 Scouting 已启用

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable scouting feature
确认此项已勾选
```

### 3. 编译

```bash
idf.py build
```

### 4. 烧录

```bash
idf.py flash
```

### 5. 监视串口输出

```bash
idf.py monitor
```

预期输出（同一网络中有 Zenoh 路由器运行时）：

```
Connecting to WiFi...OK!
Scouting...
Hello { zid: Some(0123456789ABCDEF0123456789ABCDEF), whatami: "router", locators: ["tcp/192.168.1.100:7447"] }
Dropping scout results.
```

如果没有 Zenoh 节点：

```
Connecting to WiFi...OK!
Scouting...
Did not find any zenoh process.
```

按 `Ctrl+]` 退出监视器。

---

## 测试方法

### 启动 Zenoh 路由器（同一网络）

```bash
# 在连接到同一 WiFi 的 PC 上运行
zenohd
```

Scout 将发现此路由器并打印其 Hello 消息。

### 启动 Zenoh 对等端

```bash
zenohd --mode peer --listen udp/224.0.0.225:7447
```

### 运行其他 ESP32 示例

在同一网络上运行另一块 ESP32-S3 的 `z_pub.c` 或 `z_sub.c`（如果它们处于 peer 模式或使用 scout 查找路由器），它们也会被 Scout 发现。

---

## 自定义指南

### 更改 Scout 超时

传递 Scout 配置而非 `NULL`：

```c
z_owned_scouting_config_t scout_config;
z_scouting_config_default(&scout_config);
zp_scouting_config_insert(z_loan_mut(scout_config), "timeout", "5000");
z_scout(z_config_move(&config), z_closure_hello_move(&closure), z_loan(scout_config));
```

### 按角色过滤节点

在回调中忽略某些节点类型：

```c
void callback(z_loaned_hello_t *hello, void *context) {
    z_whatami_t role = z_hello_whatami(hello);
    if (role == Z_WHATAMI_ROUTER) {
        fprinthello(stdout, hello);
        fprintf(stdout, "\n");
        (*(int *)context)++;
    }
}
```

### 保持 `app_main` 存活等待异步完成

```c
printf("Scouting...\n");
z_scout(z_config_move(&config), z_closure_hello_move(&closure), NULL);

// 等待 scout 完成（根据需要调整延迟）
sleep(2);
```

---

## 故障排除

### ❌ `Z_FEATURE_SCOUTING` 未定义

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable scouting feature
```
勾选后重新编译。

### ❌ `Did not find any zenoh process.`

| 可能原因 | 解决方法 |
|---------|----------|
| 网络中没有 Zenoh 节点 | 启动 `zenohd` 或其他 Zenoh 设备 |
| 防火墙阻止 UDP 7446 | 开放 UDP 7446 端口（scout 端口） |
| WiFi 未连接 | 检查 SSID/密码和串口输出的 WiFi 状态 |
| 不同子网 | Scout 使用多播，设备必须在同一子网 |

### ❌ 串口只显示 `Scouting...` 然后没有反应

Scout 可能在 Hello 到达之前就完成了。在 `z_scout()` 后添加 `sleep(2)`（参见自定义指南）。

### ❌ 串口输出乱码

按 ESP32-S3 的复位按钮，或在监视器中按 `Ctrl+T` → `Ctrl+Y` 复位板子。

### ❌ 编译错误：找不到 `z_scout`

检查：
1. `sdkconfig` 中 `Z_FEATURE_SCOUTING` 已启用
2. `main/CMakeLists.txt` 的 `REQUIRES` 中包含 `zenoh-pico`

---

## 参考资源

| 资源 | 链接 |
|------|------|
| Zenoh Scouting 概念 | https://zenoh.io/docs/manual/abstractions/#scouting |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF WiFi 驱动 | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html |
| ESP-IDF 事件循环 | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/esp_event.html |
| FreeRTOS 事件组 | https://www.freertos.org/event-groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## 许可证

本文档对应的源代码采用 Eclipse Public License 2.0 或 Apache License 2.0 双许可——详见文件头部。
