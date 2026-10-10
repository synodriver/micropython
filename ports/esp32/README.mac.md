# ESP32 MAC 地址接口

`esp32` 模块提供六个使用公开 ESP-IDF API 的函数，独立于 BLE5 和自动 light sleep。
这些函数提供本机 SDK 地址表查询/设置、直接读取 eFuse 与本地地址派生，不会初始化
网络接口。有参数的函数仅接受位置参数。

| 函数 | 参数与结果 | 对应 IDF API |
| --- | --- | --- |
| `mac_addr_len_get(type)` | 返回该类型的字节数，不支持时返回 `0` | `esp_mac_addr_len_get()` |
| `read_mac(type)` | 返回该类型的 `bytes` 地址 | `esp_read_mac()` |
| `iface_mac_addr_set(mac, type)` | 接受指定长度的可读缓冲区，成功返回 `None` | `esp_iface_mac_addr_set()` |
| `derive_local_mac(mac)` | 接受 6 字节缓冲区，返回新的 6 字节 `bytes` | `esp_derive_local_mac()` |
| `efuse_mac_get_default()` | 直接读取出厂 eFuse 地址，返回 6/8 字节 `bytes` | `esp_efuse_mac_get_default()` |
| `efuse_mac_get_custom()` | 直接读取自定义 eFuse 地址，返回 6/8 字节 `bytes` | `esp_efuse_mac_get_custom()` |

地址按通常显示顺序排列，例如 `b"\x02\x11\x22\x33\x44\x55"` 表示
`02:11:22:33:44:55`。不需要按 BLE HCI 内部顺序反转。

## 地址类型

所有常量都在 `esp32` 中导出，但具体支持情况取决于芯片和 SDK 配置。

| 常量 | 字节数 | 说明 |
| --- | --- | --- |
| `ESP_MAC_WIFI_STA` | 6 | 本机 Wi-Fi Station 的 SDK 地址 |
| `ESP_MAC_WIFI_SOFTAP` | 6 | 本机 Wi-Fi SoftAP 的 SDK 地址 |
| `ESP_MAC_BT` | 6 | 本机蓝牙公共地址 |
| `ESP_MAC_ETH` | 6 | 以太网 SDK 地址；不代表板上已安装以太网设备 |
| `ESP_MAC_IEEE802154` | 8 | 需要芯片支持 IEEE 802.15.4 |
| `ESP_MAC_BASE` | 6 | 派生其他接口地址所用的基地址 |
| `ESP_MAC_EFUSE_FACTORY` | 6 | 出厂 eFuse MAC，只读 |
| `ESP_MAC_EFUSE_CUSTOM` | 6 | 自定义 eFuse MAC，只读；未烧录或校验失败时可能抛异常 |
| `ESP_MAC_EFUSE_EXT` | 2 | 需要 IEEE 802.15.4 支持；设置只覆盖 SDK 缓存，不烧录 eFuse |

三个接收 `type` 参数的函数（长度查询、读取和设置）使用相同的整数转换规则：
先按 MicroPython 的 `mp_obj_get_int()` 规则转换；转换成功后，不在有效类型枚举
范围内的值抛 `ValueError`。整数转换溢出时抛 `OverflowError`（例如 `1 << 100`），
不提供整数转换的对象（例如 `None`）抛 `TypeError`。自定义整数转换方法引发的异常
按原样传出。具体转换边界由运行时实现决定，不保证覆盖机器整数的整个数学范围。
读取/设置 SDK 不支持的已知类型时抛 `OSError`。
设置时必须恰好提供表中长度，错误长度抛 `ValueError`。基地址必须是单播地址，
SDK 会拒绝第一字节最低位为 1 的基地址。SDK 的其他错误通过端口现有
`check_esp_err()` 转成 `OSError`，不会使用使芯片重启的 `ESP_ERROR_CHECK()`。

P4 的本地 MAC API 不能配置外接 ESP-Hosted 芯片的 Wi-Fi/BLE 地址；
不存在的本机类型会报不支持。其他目标也不因常量存在就自动获得相应硬件能力。

## 长度查询与直接读取 eFuse

`mac_addr_len_get(type)` 返回 SDK 地址表中该类型的长度：`6`、`8` 或 `2`。
合法但当前芯片/SDK 配置不支持的类型返回 `0`；非法参数按前述整数转换规则报错。
查询不读取、生成或缓存地址，可以在设置基地址之前用来检查类型支持情况。
类型受支持并不代表板上安装了对应外设，或接口驱动已经初始化；自定义 eFuse
类型有长度也不代表其中已经烧录有效地址。

`efuse_mac_get_default()` 和 `efuse_mac_get_custom()` 每次直接读取 eFuse，
不读取或更新 SDK 地址表缓存，不受 `ESP_MAC_BASE`、接口地址或缓存的
`ESP_MAC_EFUSE_EXT` 覆盖影响。它们不烧录 eFuse。SDK 返回读取错误、地址未烧录
或校验错误时抛 `OSError`，不会用出厂地址替代失败的自定义读取。

校验策略遵循 SDK 的芯片实现和编译配置。经典 ESP32 若启用
`CONFIG_ESP_MAC_IGNORE_MAC_CRC_ERROR=y`，factory/custom MAC 的 CRC 不匹配也可
返回成功；绑定没有额外强制 CRC 校验。其他芯片不一定使用经典 ESP32 的 version/CRC
格式，不能将这两个函数视为独立于 SDK 配置的完整性验证。

支持 IEEE 802.15.4 的芯片上，两个直接读取函数返回 **8 字节**，SDK 按
`mac48[:3] + efuse_ext + mac48[3:]` 插入实际 eFuse 中的两字节扩展字段。
其他芯片上返回 **6 字节**。这与 `read_mac(ESP_MAC_EFUSE_FACTORY/CUSTOM)`
及这两种类型的长度查询结果始终为 **6 字节**不同。
8 字节结果不能截取前 6 字节作为 MAC-48；需要还原时使用 `raw[:3] + raw[5:]`。
`derive_local_mac()` 和设置 `ESP_MAC_BASE` 仍然要求 6 字节输入。

```python
import esp32

print(esp32.mac_addr_len_get(esp32.ESP_MAC_IEEE802154))  # 支持时为 8，否则为 0
raw = esp32.efuse_mac_get_default()
print(raw)
factory_mac48 = raw if len(raw) == 6 else raw[:3] + raw[5:]

try:
    custom_raw = esp32.efuse_mac_get_custom()
except OSError as error:
    print("无法读取有效的自定义 eFuse MAC:", error)
else:
    print(custom_raw)
```

## 设置时机与缓存

在首次构造 `network.WLAN()`、启用 BLE 或初始化以太网之前设置对应地址。
构造 `WLAN` 对象本身就会初始化 Wi-Fi，不需要等到 `.active(True)`。
`iface_mac_addr_set()` 只更新 SDK 地址表，不会实时改写正在运行的驱动。
关闭接口通常也不等于拆除驱动；已初始化的 Wi-Fi 应使用现有
`wlan.config(mac=...)` 修改驱动地址，并遵守该驱动的使用条件。

`read_mac()` 会在需要时从基地址派生并缓存接口地址。设置 `ESP_MAC_BASE` 必须
早于首次读取/生成这些地址；之后修改基地址不会使已经缓存的 STA/AP/BT/ETH 或
IEEE 802.15.4 地址重新生成。单独设置一个接口地址后，它不再随基地址变化。
覆盖 `ESP_MAC_EFUSE_EXT` 也不会自动重新生成已缓存的 IEEE 802.15.4 地址。

这些设置和缓存跨 MicroPython 软复位保留，硬件复位或 deep sleep 重启后消失。
若需要长期使用自定义地址，可在 NVS 或文件中自行保存，并在每次启动早期读取后设置。
接口不会自动写 NVS，也不会烧录或修改 eFuse。

`read_mac()` 的结果是 SDK 地址表中的地址，可能与 `wlan.config("mac")` 或
`ble.config("mac")` 不同：Wi-Fi 驱动可能单独修改过地址，BLE 也可能使用随机地址或
隐私地址。需要确认实际通信地址时，应通过相应驱动接口读取。

## machine.unique_id() 的兼容性修复

ESP-IDF 的 `esp_efuse_mac_get_default()` 在支持 IEEE 802.15.4 的芯片上写入
8 字节，原 ESP32 port 的 `machine.unique_id()` 只提供 6 字节缓冲区，存在越界写。
本项目将临时缓冲区扩大到 8 字节，并检查 SDK 返回值；读取失败时抛 `OSError`，
不会返回未初始化数据。

函数仍返回 SDK 输出的前 **6 字节**，保留已有应用使用的设备标识值和返回长度。
普通芯片上的值仍为出厂 MAC-48；IEEE 802.15.4 芯片上沿用原有取值，包含扩展字段，
不能将它解释为完整 MAC-48 或完整 EUI-64。需要完整出厂地址时，使用
`efuse_mac_get_default()`；需要 MAC-48 时按前文还原，或读取
`read_mac(ESP_MAC_EFUSE_FACTORY)`。设置 SDK 基地址、接口地址或扩展字段缓存
不会改变 `machine.unique_id()` 的结果。

## 使用示例

在启动早期、尚未创建网络接口时设置基地址：

```python
import esp32

factory_mac = esp32.read_mac(esp32.ESP_MAC_EFUSE_FACTORY)
local_mac = esp32.derive_local_mac(factory_mac)
esp32.iface_mac_addr_set(local_mac, esp32.ESP_MAC_BASE)
assert esp32.read_mac(esp32.ESP_MAC_BASE) == local_mac
```

若只需指定 BLE 公共地址，在支持本机蓝牙的芯片上、首次启用 BLE 前调用：

```python
import esp32

esp32.iface_mac_addr_set(b"\x02\x11\x22\x33\x44\x55", esp32.ESP_MAC_BT)

import bluetooth
ble = bluetooth.BLE()
ble.active(True)
ble.config(addr_mode=0)  # 使用公共地址模式
print(ble.config("mac"))
```

实际应用应为设备选择互不冲突的地址。BLE 随机/隐私地址仍通过蓝牙接口管理。

`derive_local_mac()` 是确定性派生：设置第一字节的 `0x02` 位；输入已经是本地管理
地址时，再翻转 `0x04` 位。它不改变输入、不改 SDK 地址表、不生成随机地址，也不
保证任意多个输入的输出互不冲突。它保留单播/组播位，使用单播源才能得到单播结果。
IDF 内部的 `generate_mac()` 并未公开，本实现不复制其算法。

## 验证

设备用例是 `tests/ports/esp32/esp32_mac.py`，通过项目的 `tests/run-tests.py` 在
ESP32 设备上运行。用例核对长度查询、直接读取 eFuse 的 6/8 字节结果与缓存独立性、
地址表读取、长度检查、派生、覆盖及恢复、只读类型和非法参数；不保证已启动驱动
会采用新地址。还核对 `machine.unique_id()` 的六字节兼容值、重复读取及不受
地址表覆盖影响。尚未执行设备用例或测量实际通信地址。
本轮静态检查结果及边界见仓库根目录的 `MAC静态审计报告.md`。
