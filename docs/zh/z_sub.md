# z_sub.md — ESP32 (S3 / C5) Zenoh 订阅者教程

[← 返回 docs](../README.md) | [English Version](../en/z_sub.md)

## 概述

`z_sub.c` 是一个面向 **ESP32 (S3 / C5)** 的 Zenoh 订阅者（Subscriber）示例程序，运行于 **ESP-IDF v6.0** 框架之上。它演示了嵌入式设备如何通过 WiFi 加入 Zenoh 网络，订阅指定主题并接收消息。

### 核心功能

| 功能 | 说明 |
|------|------|
| WiFi STA 连接 | 连接指定 SSID 的 WiFi 热点，断开自动重试（最多 5 次） |
| Peer / Client 双模式 | 支持连接 Zenoh 路由器（Client）或直连对等端（Peer） |
| 主题订阅 | 订阅 `demo/example/**` 通配符主题，接收所有匹配的消息 |
| 错误处理 | NVS 初始化失败自动擦除重试；Zenoh 操作失败立即退出 |

### 数据流

```
[某发布者] --发布→ [Zenoh 网络] ---- TCP/UDP ---→ [ESP32 (S3 / C5)]
                                                      │
                                                data_handler()
                                                      │
                                                  printf()
                                               (串口打印消息)
```

---

## 前置条件

### 硬件

- ESP32 开发板（ESP32-S3-DevKitC-1 或 ESP32-C5-DevKitC）
- USB-C 数据线（用于供电和串口）

### 软件

| 工具 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | v6.0.1 | 嵌入式开发框架 |
| zenoh-pico | v1.9.0 | Zenoh 协议栈（小型化 C 库） |
| xtensa-esp32s3-elf-gcc | — | 交叉编译器 |
| clang-format | — | 代码格式化（可选） |

### 网络

- 一个 2.4 GHz WiFi 热点（SSID + 密码）
- 同一网络内的发布者——可以是：
  - 另一块 ESP32 运行 `z_pub.c`
  - PC 运行 `zenoh CLI` 或 `zenoh-python`
  - 任何 Zenoh 兼容的发布者

---

## 代码分节详解

以下按代码出现的顺序逐段讲解。

### 1. 版权与许可证头

```c
// Copyright (c) 2022 ZettaScale Technology
// ...
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
```

由 ZettaScale Zenoh 团队编写，采用 **Eclipse Public License 2.0** 或 **Apache License 2.0** 双许可。

### 2. 头文件包含

```c
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nvs_flash.h>
#include <unistd.h>
#include <zenoh-pico.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
```

分为四组（`.clang-format` 的 `IncludeCategories` 强制排序）：

| 组 | 头文件 | 作用 |
|----|--------|------|
| ESP-IDF | `esp_event.h` / `esp_log.h` / `esp_system.h` / `esp_wifi.h` | 事件循环、日志、系统调用、WiFi 驱动 |
| C 标准库 | `stdio.h` / `stdlib.h` / `string.h` / `unistd.h` | printf / exit / strcmp / sleep |
| 第三方 | `nvs_flash.h` / `zenoh-pico.h` | 非易失存储、Zenoh 发布/订阅协议 |
| FreeRTOS | `FreeRTOS.h` / `event_groups.h` / `task.h` | 实时内核、事件组同步、任务管理 |

### 3. 编译时守卫：`#if Z_FEATURE_SUBSCRIPTION == 1`

```c
#if Z_FEATURE_SUBSCRIPTION == 1
// ... 主体代码 ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
```

这是编译时守卫。zenoh-pico 支持按特性裁剪——如果构建时未启用订阅功能（`Z_FEATURE_SUBSCRIPTION != 1`），则 `app_main` 退化为一条错误提示，而不是编译到一半才发现符号缺失。

> 🧠 **为什么需要这个守卫？** 因为 `z_declare_subscriber`、`z_closure` 等订阅相关的函数只在启用了订阅功能时才被链接到固件中。如果没有守卫，编译会失败。这种设计让开发者针对特定场景定制最小固件体积。

### 4. WiFi 配置宏

```c
#define ESP_WIFI_SSID "SSID"
#define ESP_WIFI_PASS "PASS"
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

- `ESP_WIFI_SSID` / `ESP_WIFI_PASS` — **硬编码**的 WiFi 凭据。使用前必须修改为实际热点信息。
- `ESP_MAXIMUM_RETRY` — 断开后最大重试次数，超限则停止自动重连。
- `WIFI_CONNECTED_BIT` — FreeRTOS 事件组中的标志位，表示 DHCP 已分配 IP。

> ⚠️ **安全提示**：硬编码凭据不适用于生产环境。生产部署应从 NVS 或配置文件读取（ESP-IDF 提供了 `nvs_set_str` / `nvs_get_str` API）。

### 5. 全局状态变量

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

| 变量 | 类型 | 作用 |
|------|------|------|
| `s_is_wifi_connected` | `bool` | 事件处理器在获得 IP 时设为 `true`，`app_main` 轮询此标志 |
| `s_event_group_handler` | `EventGroupHandle_t` | FreeRTOS 事件组句柄，用于在 ISR/回调和主任务之间同步 |
| `s_retry_count` | `int` | 当前重试次数，收到 `GOT_IP` 时清零 |

### 6. Zenoh 模式选择

```c
#define CLIENT_OR_PEER 0 // 0: Client mode; 1: Peer mode
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""       // 空 LOCATOR 表示使用 scouting 自动发现
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#else
#error "Unknown Zenoh operation mode."
#endif
```

| 模式 | 宏值 | 描述 |
|------|------|------|
| **Client** | `CLIENT_OR_PEER=0` | 连接到 Zenoh 路由器（`zenohd`）。空 LOCATOR 时自动 scouting 发现路由器 |
| **Peer** | `CLIENT_OR_PEER=1` | UDP 多播直连其他对等端，无需路由器 |

#### Client 模式（`CLIENT_OR_PEER = 0`）

```
[ESP32 客户端] ---tcp/7447---> [zenohd 路由器] <---tcp/7447---> [PC 发布者]
```

- ESP32 **连接**到中央 Zenoh 路由器（`zenohd`）
- `LOCATOR` 为空时，ESP32 发送 UDP 多播 scouting 报文自动发现路由器
- 路由器负责转发消息——发布者和订阅者不直接通信
- **需要**在同一网络中运行 `zenohd`
- **推荐新手使用**

#### Peer 模式（`CLIENT_OR_PEER = 1`）

```
[ESP32 对等端] ---udp/多播---> [PC 对等端]
```

- 无需路由器——设备通过 UDP 多播互发现（地址 `224.0.0.225:7447`）
- ESP32 **监听**多播组，可收可发
- `#iface=en0` 指定网卡（macOS 为 `en0`；Linux 为 `eth0`/`wlan0`）
- 适合简单双设备场景，不如 Client 模式可扩展

#### 何时选择哪种模式

| 场景 | 推荐模式 |
|------|---------|
| 实验室 PC 运行 `zenohd` | Client（scouting） |
| ESP32 直连笔记本，无路由器 | Peer |
| 生产部署 | Client（显式指定端点） |
| Docker 容器内测试 | Client（显式 `--connect tcp/...`） |

#### 开发建议

**新手推荐 Client 模式 + scouting**：

```bash
# 终端 1：启动路由器
zenohd

# 终端 2：启动发布者
zenoh pub -k "demo/example/test" -v "Hello ESP32!"

# ESP32 上电后自动发现路由器并连接
```

**高级用法：Peer 模式**需要确保网卡名称正确（Windows 通常为 `以太网`，Linux 为 `eth0`/`wlan0`，macOS 为 `en0`）。

### 7. 订阅主题

```c
#define KEYEXPR "demo/example/**"
```

这里的关键区别是 `**`（双星号通配符）——这是 Zenoh 特有的语法：

| 通配符 | 匹配规则 | 示例 |
|--------|----------|------|
| `*` | 匹配单级路径 | `demo/example/*` 匹配 `demo/example/foo`，但不匹配 `demo/example/foo/bar` |
| `**` | 匹配多级路径 | `demo/example/**` 匹配 `demo/example/foo`、`demo/example/foo/bar`、`demo/example/a/b/c` |

因此订阅 `demo/example/**` 后，任何发布到 `demo/example/` 之下的消息都会被收到——包括后续通过 `z_pub.py` 或 `z_pub.c` 发送的 `demo/example/zenoh-pico-pub`。

---

### 8. WiFi 事件处理器

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

ESP-IDF 的事件系统将 WiFi 状态变化转发到此回调。三个关键事件：

| 事件 | 触发时机 | 处理逻辑 |
|------|----------|----------|
| `WIFI_EVENT_STA_START` | WiFi 驱动初始化完毕 | 调用 `esp_wifi_connect()` 开始关联 AP |
| `WIFI_EVENT_STA_DISCONNECTED` | 与 AP 断开 | 若重试次数未超限，重连并递增计数 |
| `IP_EVENT_STA_GOT_IP` | DHCP 获取到 IP | 设置 `WIFI_CONNECTED_BIT` 标志位，重置重试计数 |

> 💡 **为什么不用 WIFI_EVENT_STA_CONNECTED？** CONNECTED 只表示 WiFi 链路层关联成功，但还没有 IP 地址。GOT_IP 才是真正可以开始 TCP/UDP 通信的标志。如果在这之前就打开 Zenoh 会话，连接会失败。

#### 重试机制图解

```
开始  →  START → connect()
        ↓
        关联 AP ...
        ↓
   ┌──→ GOT_IP → 设置 BIT → 成功
   │      ↑
   │    重试次数 < 5？
   │      │ 是
   └── 断开 ←── 否 → 放弃
```

---

### 9. `wifi_init_sta()` — WiFi 初始化（阻塞式）

这段代码是标准的 ESP-IDF STA 初始化流程，分成 7 个逻辑步骤：

**步骤 1 — 创建事件组**
```c
s_event_group_handler = xEventGroupCreate();
```
创建 FreeRTOS 事件组——一个轻量级的标志位容器。事件处理器可以在任意上下文中设置其位，主任务可以阻塞等待。

**步骤 2 — 初始化网络子系统**
```c
esp_netif_init();                        // 创建 lwIP TCP/IP 栈
esp_event_loop_create_default();         // 启动系统事件循环
esp_netif_create_default_wifi_sta();     // 创建 STA 网络接口对象
```

三行代码完成了底层的网络基础设施搭建：
1. `esp_netif_init()` — 初始化 lwIP（轻型 TCP/IP 协议栈）
2. `esp_event_loop_create_default()` — 创建事件循环，WiFi 和 IP 事件都通过这个循环分发
3. `esp_netif_create_default_wifi_sta()` — 创建默认的 STA 网络接口，内含 DHCP 客户端

**步骤 3 — 初始化 WiFi 驱动并注册事件处理器**
```c
wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&config);

esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...);
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...);
```

关键原则：**在 `esp_wifi_start()` 之前注册事件处理器**，防止 WiFi 驱动启动后立即产生的事件被漏掉。

注意这里注册了两个独立的句柄：
- `handler_any_id` — 覆盖所有 WiFi 事件（用于 START 和 DISCONNECTED）
- `handler_got_ip` — 专门捕获 IP 层的 GOT_IP 事件（来源是 DHCP 客户端，属于 `IP_EVENT` 基类型，不是 `WIFI_EVENT`）

**步骤 4 — 配置并启动 WiFi**
```c
wifi_config_t wifi_config = {
    .sta = { .ssid = ESP_WIFI_SSID, .password = ESP_WIFI_PASS }
};
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
esp_wifi_start();
```

`WIFI_MODE_STA` 表示 Station 模式（客户端），`WIFI_IF_STA` 是前面创建的默认 STA 接口。

**步骤 5 — 阻塞等待连接**
```c
EventBits_t bits = xEventGroupWaitBits(
    s_event_group_handler, WIFI_CONNECTED_BIT,
    pdFALSE, pdFALSE, portMAX_DELAY);
```

`xEventGroupWaitBits` 是 FreeRTOS 的阻塞等待 API：
- `WIFI_CONNECTED_BIT` — 等待的位
- `pdFALSE` — 等待完成后不清除标志位
- `pdFALSE` — 等待所有指定位（这里只有一位，此参数无关）
- `portMAX_DELAY` — **无限等待**，直到事件处理器设置该位

> ⚠️ 在实际产品中建议设置超时（如 `pdMS_TO_TICKS(30000)`）加上降级逻辑，而不是无限等待。

**步骤 6 — 设置连接标志**
```c
if (bits & WIFI_CONNECTED_BIT) {
    s_is_wifi_connected = true;
}
```
供 `app_main` 中的轮询循环做二次检查。

**步骤 7 — 清理**
```c
esp_event_handler_instance_unregister(IP_EVENT, ...);
esp_event_handler_instance_unregister(WIFI_EVENT, ...);
vEventGroupDelete(s_event_group_handler);
```

连接成功后，事件处理器和事件组都不再需要，注销/删除以释放资源。但 WiFi 连接本身会继续保持。

#### 内存视角下的生命周期

```
wifi_init_sta()                     app_main()
    │                                   │
    ├── xEventGroupCreate()             │
    ├── esp_netif_init()                │
    ├── esp_wifi_init()                 │
    ├── esp_wifi_start()                │
    ├── xEventGroupWaitBits() ← 阻塞    │
    │       ↓ GOT_IP                    │
    ├── unregister handlers             │
    ├── vEventGroupDelete()             │
    └── 返回 ─────────────────────→  轮询 OK → 继续
```

---

### 10. `data_handler()` — 订阅回调函数

这是整个订阅者的核心——每当收到一条匹配 `demo/example/**` 的消息，Zenoh 就调用此函数。

```c
void data_handler(z_loaned_sample_t *sample, void *arg)
```

#### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `sample` | `z_loaned_sample_t *` | **借用的**样本引用——Zenoh 拥有内存，我们只能读取，不能 free |
| `arg` | `void *` | 用户上下文指针（本例未使用，为 NULL） |

#### 逐行解析

```c
z_view_string_t keystr;
z_keyexpr_as_view_string(z_sample_keyexpr(sample), &keystr);
```
从样本中提取 key expression（主题路径）。`z_view_string_t` 是"视图"类型——它不拥有字符串的所有权，只是指向 Zenoh 内部数据的指针+长度。

```c
z_owned_string_t value;
z_bytes_to_string(z_sample_payload(sample), &value);
```
将二进制的 payload 转换为 C 字符串。与上面的视图不同，`z_owned_string_t` 是**拥有**类型——`z_bytes_to_string` 在堆上分配了内存，使用完后必须释放。

```c
printf(" >> [Subscriber handler] Received ('%.*s': '%.*s')\n",
       (int)z_string_len(z_view_string_loan(&keystr)),
       z_string_data(z_view_string_loan(&keystr)),
       (int)z_string_len(z_string_loan(&value)),
       z_string_data(z_string_loan(&value)));
```

`printf` 使用的 `%.*s` 格式说明符：
- `.*` — 从参数中读取字符串长度
- `%s` — 从参数中读取字符串指针

因为视图/拥有字符串可能包含非 `\0` 结尾的二进制数据（字符串的长度是明确存储的），所以要用 `len + data` 两个参数安全打印。

```c
z_string_drop(z_string_move(&value));
```
释放之前分配的拥有字符串。`z_string_move` 将所有权从 `value` 转移到临时值，`z_string_drop` 释放内存。

> 💡 **为什么需要手动管理内存？** zenoh-pico 是一个嵌入式 C 库，没有垃圾回收。它的类型系统用"拥有/借用"来区分所有权：`z_owned_*` 类型必须被 drop，`z_view_*` / `z_loaned_*` 类型只是借用，不能 drop。

---

### 11. `app_main()` — 主入口函数（7 步流程）

```c
void app_main()
```

ESP32 上电 → ROM bootloader → ESP-IDF 启动代码 → **调用 app_main()**。它运行在一个独立的 FreeRTOS 任务中。

完整流程：

```
┌───────────────────────────────────┐
│ Step 1: NVS 初始化                │
│ nvs_flash_init()                  │
│ (WiFi 校准数据 / DHCP 配置)       │
└──────────┬────────────────────────┘
           ↓
┌───────────────────────────────────┐
│ Step 2: WiFi 连接                 │
│ wifi_init_sta()                   │
│ ⏳ 阻塞直到 DHCP 分配 IP           │
└──────────┬────────────────────────┘
           ↓
┌───────────────────────────────────┐
│ Step 3: Zenoh 配置                │
│ z_config_default()                │
│ 设置 mode / connect / listen      │
└──────────┬────────────────────────┘
           ↓
┌───────────────────────────────────┐
│ Step 4: 打开会话                  │
│ z_open()                          │
│ ⚡ TCP 连接到路由器 / 加入多播     │
└──────────┬────────────────────────┘
           ↓
┌───────────────────────────────────┐
│ Step 5: 声明订阅                  │
│ z_declare_subscriber()            │
│ + data_handler 回调               │
└──────────┬────────────────────────┘
           ↓
┌───────────────────────────────────┐
│ Step 6: 空闲循环                  │
│ while(1) { sleep(1); }            │
│ ↺ data_handler 异步接收           │
└──────────┬────────────────────────┘
           ↓ (永不执行 ↓)
┌───────────────────────────────────┐
│ Step 7: 清理                      │
│ z_drop(sub) → z_drop(session)     │
│ (仅为教学展示)                    │
└───────────────────────────────────┘
```

#### 11.1 Step 1 — NVS 初始化

```c
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);
```

NVS（Non-Volatile Storage）是 ESP-IDF 用于存储持久化数据的 Flash 分区。WiFi 驱动依赖它来读取校准数据（MAC 地址、RF 参数等）。两个需要擦除重试的特殊错误：

| 错误码 | 含义 | 典型场景 |
|--------|------|----------|
| `ESP_ERR_NVS_NO_FREE_PAGES` | NVS 分区空间耗尽 | 频繁写入未清理的历史数据 |
| `ESP_ERR_NVS_NEW_VERSION_FOUND` | NVS 格式版本不匹配 | 升级 ESP-IDF 后 NVS 布局改变 |

#### 11.2 Step 2 — WiFi 连接

```c
printf("Connecting to WiFi...");
wifi_init_sta();
while (!s_is_wifi_connected) {
    printf(".");
    sleep(1);
}
printf("OK!\n");
```

`wifi_init_sta()` 内部已经通过事件组阻塞，这里额外套了一层 `while` 循环做**双重保障**——如果 FreeRTOS 事件组机制因某种极端情况未能唤醒主任务，这个轮询可以提供降级。

打印的 `.` 数量反映了实际连接耗时：

```
Connecting to WiFi............OK!   ← 3-5 秒，正常
Connecting to WiFi.OK!              ← <1 秒，很快（可能是重连）
```

#### 11.3 Step 3 — Zenoh 配置

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

这个配置块根据 `CLIENT_OR_PEER` 宏产生不同的行为：

| 场景 | LOCATOR | 效果 |
|------|---------|------|
| Client + 空 LOCATOR | `""` | Zenoh 通过 UDP 多播发送 scouting 查询，自动发现网络中的路由器 |
| Client + 指定地址 | `"tcp/<ROUTER_IP>:7447"` | 直接连接指定端点，更可靠（无多播依赖） |
| Peer | `"udp/224.0.0.225:7447#iface=en0"` | 监听多播地址，与其他 peer 直连 |

> 🧠 **`z_loan_mut` 的作用**：Zenoh 的类型系统区分"拥有"和"借用"。`z_owned_config_t` 是拥有类型，要修改它需要先借出可变引用——`z_loan_mut(config)` 就是这个用途。在传统 C 中它等价于 `&config`，但显式的 API 让所有权关系一目了然。

#### 11.4 Step 4 — 打开 Zenoh 会话

```c
z_owned_session_t s;
if (z_open(&s, z_move(config), NULL) < 0) {
    printf("Unable to open session!\n");
    exit(-1);
}
```

`z_open()` 做了以下工作：
1. 解析配置（mode、端点等）
2. 建立传输层连接（TCP 连接或多播加入）
3. 进行 Zenoh 协议握手（版本协商、能力交换）
4. 返回会话句柄

`z_move(config)` 将配置的所有权"移动"到会话中——`z_open` 成功后 `config` 不再有效。这是 Zenoh 的**移动语义**，类似于 C++ 的 `std::move`，避免了配置数据的不必要拷贝。

失败时（路由器不可达、网络未就绪、防火墙阻止），调用 `exit(-1)` 使 FreeRTOS 重启任务。

#### 11.5 Step 5 — 声明订阅者

```c
z_owned_closure_sample_t callback;
z_closure(&callback, data_handler, NULL, NULL);

z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);

if (z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(callback), NULL) < 0) {
    printf("Unable to declare subscriber.\n");
    exit(-1);
}
```

这部分包含几个 Zenoh 特有的概念：

**闭包（Closure）**
```c
z_owned_closure_sample_t callback;
z_closure(&callback, data_handler, NULL, NULL);
//                      ↑ 回调函数  ↑ 上下文  ↑ 释放函数
```
`z_closure` 创建了一个三成员闭包：
1. `data_handler` — 消息到达时调用的函数
2. `NULL` — 用户上下文指针（可在回调的 `arg` 参数中接收）
3. `NULL` — 闭包销毁时的资源释放函数

**Key Expression 视图**
```c
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
```
`z_view_keyexpr_t` 是一个"视图"类型——它不拥有字符串，只是指向 `"demo/example/**"` 的指针。`_unchecked` 后缀跳过了 key expression 的有效性验证（比如检查是否包含非法字符），适合编译期已知的常量字符串。

**声明订阅**
```c
z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(callback), NULL)
```

| 参数 | 类型 | 作用 |
|------|------|------|
| `z_loan(s)` | 借用会话 | 指定在哪个会话上订阅 |
| `&sub` | 输出参数 | 接收订阅者句柄 |
| `z_loan(ke)` | 借用 key expr | 订阅的主题路径 |
| `z_move(callback)` | 移动闭包 | 闭包所有权转移给订阅者（之后不可再用） |
| `NULL` | 附加配置 | 可传入 `z_subscriber_options_t`，NULL 表示默认选项 |

`z_move(callback)` 之后，`callback` 变量不再有效——Zenoh 内部接管了它的生命周期。

#### 11.6 Step 6 — 空闲循环

```c
while (1) {
    sleep(1);
}
```

这是最精简的主循环——订阅者的核心工作（接收消息、触发回调）完全在后台由 Zenoh 的传输层完成。FreeRTOS 的调度器在 `sleep(1)` 期间会运行其他任务（包括 WiFi 和 TCP/IP 栈的守护任务）。

> 🤔 **为什么不需要轮询？** 这是事件驱动的架构 vs 轮询架构的区别：
> - **发布者**需要主动 `z_publisher_put()`（轮询/定时驱动）
> - **订阅者**是被动的：Zenoh 内部线程收到数据包后直接调用 `data_handler`（事件驱动）

如果需要做更多工作（如解析 JSON、控制 GPIO、更新 LED），直接在 `data_handler` 中添加即可——但不要在回调中执行耗时操作，以免阻塞 Zenoh 的接收线程。耗时操作应通过 FreeRTOS 队列传递给其他任务。

#### 11.7 Step 7 — 清理（仅教学展示）

```c
printf("Closing Zenoh Session...");
z_drop(z_move(sub));
z_drop(z_move(s));
printf("OK!\n");
```

这段代码在当前的无限循环中永远不会执行。它的存在是为了说明正确的资源释放顺序：

1. **先释放订阅者**（`z_drop(sub)`）— 停止接收消息
2. **再关闭会话**（`z_drop(s)`）— 断开传输连接

顺序不能反——如果先关闭会话再释放订阅者，订阅者可能还在尝试使用已关闭的会话。这也是 RAII / 资源获取即初始化的通用原则：**析构顺序与构造顺序相反**。

在真实产品中，如果需要在运行时停止订阅，可以这样改造：

```c
// 将 sub 声明为全局
static z_owned_subscriber_t g_sub;

void stop_subscriber(void) {
    z_drop(z_move(g_sub));           // 先停止订阅
    printf("Subscriber stopped.\n");
}

void app_main() {
    // ... 步骤 1-5 ...
    while (keep_running) {           // 用条件变量替代 while(1)
        sleep(1);
    }
    z_drop(z_move(g_sub));
    z_drop(z_move(s));
}
```

---

### 12. 编译时回退分支

```c
#else
void app_main() {
    printf("ERROR: Zenoh pico was compiled without Z_FEATURE_SUBSCRIPTION but "
           "this example requires it.\n");
}
#endif /* Z_FEATURE_SUBSCRIPTION */
```

当 `Z_FEATURE_SUBSCRIPTION == 0` 时，zenoh-pico 库中没有订阅功能相关的符号，如果直接编译 `app_main` 的主体代码，链接器会报"未定义引用"错误。用守卫包裹后，编译器选择这段回退代码，仅打印一条错误提示后退出。

**如何启用订阅功能：**

```bash
idf.py menuconfig
# → Component config → Zenoh pico → Enable subscription feature → 勾选
# 保存退出
idf.py build
```

---

## 典型用法示例

### 场景 1：ESP32 订阅 + Python 发布（推荐）

使用本项目中的 `scripts/z_pub.py`：

```bash
pip install zenoh

# ESP32 上电（自动连接 WiFi + Zenoh）

# PC 上运行 Python 发布者
uv run python3 scripts/z_pub.py "Hello from Python!"

# ESP32 串口输出：
# >> [Subscriber handler] Received ('demo/example/zenoh-pico-pub': 'Hello from Python!')
```

### 场景 2：两个 ESP32 互发（Peer 模式）

将两块 ESP32 的 `CLIENT_OR_PEER` 都设为 1，确保网卡名称匹配。一块运行 `z_pub.c`，另一块运行 `z_sub.c`，它们直接通过 UDP 多播通信，无需路由器。

---

## 构建与烧录

### 1. 配置 WiFi 凭据

编辑 `main/z_sub.c`，修改宏定义：

```c
#define ESP_WIFI_SSID "你的热点名称"
#define ESP_WIFI_PASS "你的热点密码"
```

### 2. 选择 Zenoh 模式（可选）

```c
#define CLIENT_OR_PEER 0   // Client 模式（推荐）
#define CLIENT_OR_PEER 1   // Peer 模式
```

### 3. 选择连接方式

**Client 模式**：需要启动 `zenohd`：

```bash
# 终端 1：启动路由器
zenohd

# 终端 2：发送测试消息
zenoh pub -k "demo/example/test" -v "hello"
```

**Client 模式 + 手动端点**：修改 `LOCATOR`：

```c
#define LOCATOR "tcp/192.168.1.100:7447"  // 替换为路由器的实际 IP
```

### 4. 构建、烧录、监视

```bash
# 一键完成
idf.py build flash monitor

# 或分步执行
idf.py build
idf.py flash
idf.py monitor
```

### 预期输出

```
Connecting to WiFi....OK!
Opening Zenoh Session...OK
Declaring Subscriber on 'demo/example/**'...OK!
```

此时如果有发布者在同一 Zenoh 网络中发送消息，会实时显示：

```
 >> [Subscriber handler] Received ('demo/example/zenoh-pico-pub': 'Hello from Python!')
 >> [Subscriber handler] Received ('demo/example/test': 'hello')
```

按 `Ctrl+]` 退出监视器。

---

## 自定义指南

### 修改订阅主题

```c
#define KEYEXPR "sensor/temperature/#"
```

Zenoh 支持的通配符：

| 通配符 | 含义 | 示例 |
|--------|------|------|
| `*` | 匹配一个路径段 | `sensor/*/temp` 匹配 `sensor/room1/temp` |
| `**` | 匹配任意多个路径段 | `sensor/**` 匹配 `sensor/room1/temp` 和 `sensor/floor2/room1/temp` |

### 在回调中做更多事情

```c
void data_handler(z_loaned_sample_t *sample, void *arg) {
    z_owned_string_t value;
    z_bytes_to_string(z_sample_payload(sample), &value);

    const char *payload = z_string_data(z_string_loan(&value));

    if (strcmp(payload, "LED_ON") == 0) {
        gpio_set_level(LED_PIN, 1);
    } else if (strcmp(payload, "LED_OFF") == 0) {
        gpio_set_level(LED_PIN, 0);
    }

    z_string_drop(z_string_move(&value));
}
```

### 使用回调上下文指针

```c
typedef struct {
    int message_count;
    int last_rssi;
} sub_context_t;

static sub_context_t ctx = {0, 0};

void data_handler(z_loaned_sample_t *sample, void *arg) {
    sub_context_t *c = (sub_context_t *)arg;
    c->message_count++;
    printf("Received message #%d\n", c->message_count);
}

void app_main() {
    // ...
    z_owned_closure_sample_t callback;
    z_closure(&callback, data_handler, &ctx, NULL);  // 传入上下文
    // ...
}
```

### 修改发送者行为

当收到特定命令时，发送响应——但要注意，在 `data_handler` 中直接发送需要访问会话句柄，需要通过上下文传递：

```c
typedef struct {
    z_owned_session_t *session;
    int count;
} ctx_t;

void data_handler(z_loaned_sample_t *sample, void *arg) {
    ctx_t *c = (ctx_t *)arg;
    c->count++;

    // 发送回复
    z_owned_bytes_t reply;
    z_bytes_from_str(&reply, "ACK");
    z_put(c->session, z_loan(ke_reply), z_move(reply), NULL);
}
```

---

## 常见问题排查

### ❌ `Unable to open session!`

| 可能原因 | 解决方法 |
|----------|----------|
| Client 模式：未启动 `zenohd` | 在终端运行 `zenohd` |
| WiFi 连接失败 | 检查 SSID/密码是否正确 |
| 防火墙屏蔽 Zenoh 端口 | 开放 TCP 7447（Client）或 UDP 多播（Peer） |
| Peer 模式网卡名称不匹配 | 将 `en0` 改为实际网卡名 |
| 路由器 IP 变更 | 使用 scouting（LOCATOR 留空） |

### ❌ `Unable to declare subscriber.`

通常在会话打开后立即发生——说明会话虽然建立了，但在注册订阅时失败：

- 网络不稳定导致会话已断开
- 路由器限制了订阅数量或权限
- 内存不足（`z_owned_closure_sample_t` 分配失败）

### ❌ 串口打印了 "Connecting to WiFi..." 但之后没有输出

WiFi 连接卡住了——最可能是 SSID/密码错误。可尝试：

1. 确认热点是 2.4 GHz（ESP32-S3 不支持 5 GHz；ESP32-C5 支持 5 GHz 但本例配置为 2.4 GHz）
2. 检查密码和加密方式（推荐 WPA2-PSK）
3. 增大 `ESP_MAXIMUM_RETRY` 或添加串口调试打印

### ❌ 收到消息但打印乱码

```c
printf(" >> [Subscriber handler] Received ('%.*s': '%.*s')\n",
       (int)z_string_len(z_view_string_loan(&keystr)),
       z_string_data(z_view_string_loan(&keystr)),
       (int)z_string_len(z_string_loan(&value)),
       z_string_data(z_string_loan(&value)));
```

确保使用 `%.*s` + `z_string_len` 的形式，而不是直接用 `%s`。Zenoh 的字符串不是以 `\0` 结尾的，`%s` 会读到垃圾数据。

### ❌ 编译时报错 `Z_FEATURE_SUBSCRIPTION` 未定义

```bash
idf.py menuconfig
# → Component config → Zenoh pico → Enable subscription feature
# 确认勾选
```

### ❌ ESP32 不停地重启

观察到以下输出循环出现：

```
Connecting to WiFi....OK!
Opening Zenoh Session...Unable to open session!
```

`exit(-1)` 导致 FreeRTOS 任务终止，ESP-IDF 的看门狗触发系统重启。检查路由器是否运行且可达。

---

## 与 `z_pub.c` 的对比

| 维度 | `z_pub.c`（发布者） | `z_sub.c`（订阅者） |
|------|--------------------|--------------------|
| 编译开关 | `Z_FEATURE_PUBLICATION` | `Z_FEATURE_SUBSCRIPTION` |
| 核心操作 | `z_declare_publisher` + `z_publisher_put` | `z_declare_subscriber` + 回调 |
| 运行模式 | 主动推送（每 1 秒） | 被动等待（事件驱动） |
| 主循环 | `sleep(1)` → put → 循环 | `sleep(1)` 空转 |
| Key Expression | `demo/example/zenoh-pico-pub`（固定路径） | `demo/example/**`（通配符） |
| 内存管理 | 每次 put 创建 + drop payload | 每次回调创建 + drop value string |

---

## 参考资源

| 资源 | 链接 |
|------|------|
| Zenoh 官方文档 | https://zenoh.io/docs/ |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF 编程指南 | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/ |
| FreeRTOS 事件组 | https://www.freertos.org/FreeRTOS-Event-Groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## 许可证

本文档对应源码使用 Eclipse Public License 2.0 或 Apache License 2.0 双许可，见源文件头部。
