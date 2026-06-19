# z_pub.c — ESP32-S3 Zenoh 发布者教程

## 概述

`z_pub.c` 是一个面向 **ESP32-S3** 的 Zenoh 发布者（Publisher）示例程序，运行于 **ESP-IDF v6.0** 框架之上。它演示了嵌入式设备通过 WiFi 加入 Zenoh 网络，并以固定频率发布消息的完整流程。

### 核心功能

| 功能 | 说明 |
|------|------|
| WiFi STA 连接 | 连接指定 SSID 的 WiFi 热点，自动重试（最多 5 次） |
| Zenoh 会话 | 以 Client 或 Peer 模式打开 Zenoh 会话 |
| 消息发布 | 每秒发布一条递增计数消息到指定主题（key expression） |
| 错误处理 | NVS 初始化失败自动擦除重试；Zenoh 操作失败立即退出 |

### 数据流

```
[ESP32-S3] --- WiFi STA ---> [WiFi AP] ----> [Zenoh 网络] ---> (订阅者收消息)
    │                                   │
    │ 每 1 秒                           │
    │ 发布 "[N] [ESPIDF]{ESP32} ..."    │
    └───────────────────────────────────┘
```

---

## 前置条件

### 硬件

- ESP32-S3 开发板（如 ESP32-S3-DevKitC-1）
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
- 同一网络内运行一个 Zenoh 路由器（`zenohd`）或订阅者（Subscriber），用于接收消息

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

#include <nvs_flash.h>
#include <zenoh-pico.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
```

分为四组（.clang-format `IncludeCategories` 强制排序）：

| 组 | 头文件 | 作用 |
|----|--------|------|
| ESP-IDF | `esp_event.h` / `esp_log.h` / `esp_system.h` / `esp_wifi.h` | 事件循环、日志、系统调用、WiFi 驱动 |
| C 标准库 | `stdio.h` / `stdlib.h` / `string.h` / `unistd.h` | printf / exit / strcmp / sleep |
| 第三方 | `nvs_flash.h` / `zenoh-pico.h` | 非易失存储、Zenoh 发布/订阅协议 |
| FreeRTOS | `FreeRTOS.h` / `event_groups.h` / `task.h` | 实时内核、事件组同步、任务管理 |

### 3. 编译时开关：`#if Z_FEATURE_PUBLICATION == 1`

```c
#if Z_FEATURE_PUBLICATION == 1
// ... 主体代码 ...
#else
void app_main()
{
    printf("ERROR: ...\n");
}
#endif
```

这是一个**编译时守卫**。zenoh-pico 支持按特性裁剪——如果构建时未启用发布功能（`Z_FEATURE_PUBLICATION != 1`），则 `app_main` 退化为一条错误提示，避免链接失败。**首次使用请确认 `sdkconfig` 中启用了发布功能。**

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

> ⚠️ **安全提示**：硬编码凭据不适用于生产环境。生产部署应从 NVS 或配置文件读取。

### 5. 全局状态变量

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

- `s_is_wifi_connected` — 由事件处理器在 `GOT_IP` 时设为 `true`，`app_main` 轮询此标志直到连接成功。
- `s_event_group_handler` — FreeRTOS 事件组句柄，用于在 ISR/事件回调和主任务之间同步。
- `s_retry_count` — 当前重试次数，在 `GOT_IP` 时清零。

### 6. Zenoh 模式配置

```c
#define CLIENT_OR_PEER 0 // 0: Client 模式; 1: Peer 模式
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#else
#error "Unknown Zenoh operation mode."
#endif
```

两种模式的区别：

| 模式 | 值 | 说明 |
|------|-----|------|
| **Client** | `CLIENT_OR_PEER=0` | 作为客户端连接到一个 Zenoh 路由器（`zenohd`）。需要网络中有一个正在运行的路由器。`LOCATOR` 为空时，通过 UDP 多播自动发现（scouting）。 |
| **Peer** | `CLIENT_OR_PEER=1` | 通过 UDP 多播直接与其他端设备对等通信，无需路由器。`LOCATOR` 指定多播地址和网卡（需根据实际环境修改 `en0`）。 |

根据经验：**新手建议从 Client 模式开始**，先启动 `zenohd` 再运行设备。

### 7. 主题与消息模板

```c
#define KEYEXPR "demo/example/zenoh-pico-pub"
#define VALUE "[ESPIDF]{ESP32} Publication from Zenoh-Pico!"
```

- `KEYEXPR` — Zenoh key expression（主题路径），订阅方使用同一个 key expression 即可接收。
- `VALUE` — 消息模板，运行时会在前面加上 `[序号]`。

### 8. WiFi 事件处理函数

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

ESP-IDF 的事件系统将 WiFi 状态变化转发到此回调。三个关键事件：

| 事件 | 触发时机 | 处理逻辑 |
|------|----------|----------|
| `WIFI_EVENT_STA_START` | WiFi 驱动初始化完毕 | 调用 `esp_wifi_connect()` 开始连接 |
| `WIFI_EVENT_STA_DISCONNECTED` | 与 AP 断开连接 | 如果重试次数未超限，再次调用 `esp_wifi_connect()` 并递增计数器 |
| `IP_EVENT_STA_GOT_IP` | DHCP 分配到 IP 地址 | 设置事件组标志 `WIFI_CONNECTED_BIT` 唤醒 `app_main`，重置重试计数 |

> 其中 `handler_got_ip` 和 `handler_any_id` 两个句柄在连接完成后被注销，以避免空闲时不必要的回调。

### 9. `wifi_init_sta()` — WiFi 初始化

这段代码是标准的 ESP-IDF STA 初始化流程，分为 5 步：

**步骤 1 — 创建事件组**
```c
s_event_group_handler = xEventGroupCreate();
```
用于阻塞等待 WiFi 连接成功。

**步骤 2 — 初始化网络接口**
```c
esp_netif_init();
esp_event_loop_create_default();
esp_netif_create_default_wifi_sta();
```
创建默认事件循环和 WiFi STA 网络接口。

**步骤 3 — 初始化 WiFi 驱动**
```c
wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&config);
```
使用默认配置初始化 WiFi 驱动。

**步骤 4 — 注册事件处理器**
```c
esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...);
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...);
```
在启动 WiFi 之前注册回调，防止丢失早期事件。

**步骤 5 — 配置并启动**
```c
wifi_config_t wifi_config = {
    .sta = { .ssid = ESP_WIFI_SSID, .password = ESP_WIFI_PASS }
};
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
esp_wifi_start();
```

**阻塞等待：**
```c
xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT,
                    pdFALSE, pdFALSE, portMAX_DELAY);
```
`portMAX_DELAY` 表示**无限等待**——直到 DHCP 分配 IP 或系统重置。

**收尾清理：**
```c
esp_event_handler_instance_unregister(...)  // 注销回调
vEventGroupDelete(s_event_group_handler);    // 删除事件组
```
连接成功后不再需要这些资源。

### 10. `app_main()` — 入口函数

作为 FreeRTOS 应用程序的入口，`app_main` 按顺序执行以下流程：

```
┌─────────────────────────────┐
│  初始化 NVS                 │
│  (nvs_flash_init)           │
└──────────┬──────────────────┘
           ↓
┌─────────────────────────────┐
│  连接 WiFi                  │
│  (wifi_init_sta)            │
│  阻塞直到 GOT_IP            │
└──────────┬──────────────────┘
           ↓
┌─────────────────────────────┐
│  打开 Zenoh 会话            │
│  (z_open)                   │
│  Client/Peer 模式由宏控制   │
└──────────┬──────────────────┘
           ↓
┌─────────────────────────────┐
│  声明 Publisher             │
│  (z_declare_publisher)      │
│  KEYEXPR → "demo/example/"  │
└──────────┬──────────────────┘
           ↓
┌─────────────────────────────┐
│  无限循环发布               │
│  sleep(1) → publish → 计数  │
└─────────────────────────────┘
```

#### 10.1 NVS 初始化

```c
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);
```

NVS（Non-Volatile Storage）是 ESP-IDF 用来存储 WiFi 校准数据、MAC 地址等信息的 Flash 分区。两个特殊错误需要擦除后重试：
- `ESP_ERR_NVS_NO_FREE_PAGES` — 分区空间耗尽
- `ESP_ERR_NVS_NEW_VERSION_FOUND` — NVS 格式版本不匹配（通常出现在升级 IDF 后）

#### 10.2 WiFi 连接（带轮询回退）

```c
printf("Connecting to WiFi...");
wifi_init_sta();
while (!s_is_wifi_connected) {
    printf(".");
    sleep(1);
}
printf("OK!\n");
```

尽管 `wifi_init_sta()` 内部已通过事件组阻塞等待，这里仍用轮询作为**二次保障**。如果事件机制因某些边缘情况未能如期唤醒，此循环确保不会在 WiFi 未就绪时继续执行。

#### 10.3 构建 Zenoh 配置

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

- `z_owned_config_t` — Zenoh 的"拥有"类型，表示该配置的内存由当前作用域管理。
- `z_loan_mut(config)` — 从拥有类型中借出可变的引用。
- `Z_CONFIG_MODE_KEY` — 设置 "client" 或 "peer"。
- 如果 `LOCATOR` 非空，根据模式决定是 `Z_CONFIG_CONNECT_KEY`（连接目标）还是 `Z_CONFIG_LISTEN_KEY`（监听端点）。

在 Client 模式下 `LOCATOR` 为空时，zenoh-pico 会通过 **UDP 多播 scouting** 自动发现网络中的路由器——无需手动指定地址。

#### 10.4 打开 Zenoh 会话

```c
z_owned_session_t s;
if (z_open(&s, z_move(config), NULL) < 0) {
    printf("Unable to open session!\n");
    exit(-1);
}
```

- `z_move(config)` — 将配置的**所有权**转移到会话中。`z_open` 成功后 `config` 不再有效。这符合 Zenoh 的移动语义——避免不必要的拷贝。
- 失败时（如网络不可达、无路由器）直接 `exit(-1)`。

#### 10.5 声明 Publisher

```c
z_owned_publisher_t pub;
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
if (z_declare_publisher(z_loan(s), &pub, z_loan(ke), NULL) < 0) {
    printf("Unable to declare publisher for key expression!\n");
    exit(-1);
}
```

- `z_view_keyexpr_t` — 一个**视图类型**，它不拥有字符串，只是对 `KEYEXPR` 的引用。`_unchecked` 后缀跳过了 key expression 字符串的合法性检查。
- `z_declare_publisher` — 向 Zenoh 网络注册此节点为该主题的发布者。这一步通过网络与路由器（或直连的对等端）协商，失败通常意味着网络问题。

#### 10.6 发布循环

```c
char buf[256];
for (int idx = 0; 1; ++idx) {
    sleep(1);
    sprintf(buf, "[%4d] %s", idx, VALUE);
    printf("Putting Data ('%s': '%s')...\n", KEYEXPR, buf);

    z_owned_bytes_t payload;
    z_bytes_copy_from_str(&payload, buf);
    z_publisher_put(z_loan(pub), z_move(payload), NULL);
}
```

这是核心发布逻辑：

1. `sleep(1)` — 每秒发布一次
2. `sprintf(buf, "[%4d] %s", idx, VALUE)` — 组合消息，例如 `" [  42] [ESPIDF]{ESP32} Publication from Zenoh-Pico!"`
3. `z_bytes_copy_from_str` — 将字符串拷贝到 Zenoh 的 `Bytes` 类型中
4. `z_publisher_put` — 将数据发布到 key expression，订阅者将收到此负载

> `for (int idx = 0; 1; ++idx)` 是一个无限循环——条件部分 `1` 永远为真。注释中 `// Unreachable` 之后的清理代码在正常运行时不会执行。

---

## 构建与烧录

### 1. 配置 WiFi 凭据

编辑 `main/z_pub.c`，修改宏定义：

```c
#define ESP_WIFI_SSID "你的热点名称"
#define ESP_WIFI_PASS "你的热点密码"
```

### 2. 选择 Zenoh 模式（可选）

```c
#define CLIENT_OR_PEER 0   // Client 模式（推荐）
#define CLIENT_OR_PEER 1   // Peer 模式
```

**Client 模式**需要在同一网络中运行 `zenohd`：

```bash
# 终端 1：启动 Zenoh 路由器
zenohd
```

**Peer 模式**不需要路由器，但要注意多播地址和网卡名称需与环境匹配。

### 3. 构建

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

预期输出示例：

```
Connecting to WiFi....OK!
Opening Zenoh Session...OK
Declaring publisher for 'demo/example/zenoh-pico-pub'...OK
Putting Data ('demo/example/zenoh-pico-pub': '[   0] [ESPIDF]{ESP32} Publication from Zenoh-Pico!')...
Putting Data ('demo/example/zenoh-pico-pub': '[   1] [ESPIDF]{ESP32} Publication from Zenoh-Pico!')...
Putting Data ('demo/example/zenoh-pico-pub': '[   2] [ESPIDF]{ESP32} Publication from Zenoh-Pico!')...
```

按 `Ctrl+]` 退出监视器。

---

## 接收消息（订阅端）

要接收此示例发布的消息，将 `z_sub.c` 烧录到同一网络中的另一块 ESP32-S3 上 — 它会订阅 `demo/example/**` 并打印此发布者发送的每条消息。

或者在同一网络的 PC 上运行任意 Zenoh 订阅者。

---

## 自定义指南

### 修改发布频率

将 `sleep(1)` 改为其他值（单位：秒）：

```c
sleep(5);  // 每 5 秒发布一次
```

对于毫秒级间隔，使用 FreeRTOS 的 `vTaskDelay(pdMS_TO_TICKS(500))` 代替 `sleep`。

### 修改消息内容

```c
#define VALUE "[ESPIDF]{ESP32} 温度传感器数据"
```

或在循环中使用动态数据：

```c
float temp = read_temperature_sensor();
sprintf(buf, "{\"temp\": %.2f, \"idx\": %d}", temp, idx);
```

### 修改主题

```c
#define KEYEXPR "sensor/temperature/room1"
```

确保订阅方使用相同的 key expression。

### 使用 Scouting vs 手动指定端点

- **Scouting（自动发现，默认）**：`LOCATOR` 留空即可。路由器需启用 scouting 响应。
- **手动指定**：设置 `LOCATOR` 为路由器地址，例如：
  ```c
  #define LOCATOR "tcp/192.168.1.100:7447"
  ```
  这种方式更可靠，不会受多播网络限制。

---

## 常见问题排查

### ❌ `Unable to open session!`

| 可能原因 | 解决方法 |
|----------|----------|
| 未启动 `zenohd`（Client 模式） | 在终端运行 `zenohd` |
| WiFi 连接失败 | 检查 SSID/密码是否正确 |
| 防火墙屏蔽 UDP 7447 | 开放端口或多播权限 |
| Peer 模式网卡名称不对 | 将 `en0` 改为实际网卡名（如 `eth0`、`wlan0`、`以太网`） |

### ❌ `Unable to declare publisher for key expression!`

通常在会话打开后立即发生，可能原因：
- 路由器端权限限制
- 网络不稳定导致会话已断开

### ❌ 串口输出乱码

```
ESP-ROM:esp32s3-xxxxxxxx
```
检查 `idf.py monitor` 的波特率是否正确（默认 115200），或按 `Ctrl+T` → `Ctrl+Y` 组合键复位。

### ❌ 编译报错 `Z_FEATURE_PUBLICATION` 未定义

确保 `sdkconfig` 中已启用：

```bash
idf.py menuconfig
# → 进入 "Component config → Zenoh pico → Enable publication feature"
# 确保此项为勾选状态
```

---

## 参考资源

| 资源 | 链接 |
|------|------|
| Zenoh 官方文档 | https://zenoh.io/docs/ |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF 编程指南 | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/ |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## 许可证

本文档对应源码使用 Eclipse Public License 2.0 或 Apache License 2.0 双许可，见源文件头部。
