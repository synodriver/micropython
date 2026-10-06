# ESP32 BLE 5 实现及审计

本次代码改动全部位于 `ports/esp32`。BLE 5 新接口扩展现有
`bluetooth.BLE` 单例，继续使用同一套连接句柄、GATT、配对和 IRQ 回调。
不引入独立的 `bluetooth5` 模块。

## 芯片与构建

ESP32-S3、C2、C3、C5、C6、H2 的构建增加 `boards/sdkconfig.ble5`，
开启 2M/Coded PHY、扩展广播和周期广播。
新增代码同时受 `SOC_BLE_50_SUPPORTED`、NimBLE 配置和 MicroPython
蓝牙配置约束。原始 ESP32、S2、P4 的配置和蓝牙源码选择保持原样；
P4 即使使用外部控制器，也不会进入此次适配。

已有构建目录的 `sdkconfig` 会优先于 SDK 默认值。首次测试请使用新构建目录，
或者在已有配置中显式启用 `sdkconfig.ble5` 列出的选项。
`ble.ble5_features()` 返回固件启用的 PHY 位掩码、扩展广播/周期广播开关、
广播实例总数和配置的广播数据上限。实例总数包括预留的实例 0；SDK 选项
`CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=2` 表示额外两个实例，总共三个。
这些是固件配置能力；控制器仍会验证具体操作，连接 PHY 也需要对端支持。

共享 `extmod` 没有可用的方法表扩展钩子，因此 `ble5/make_bindings.py`
在 CMake 配置阶段生成构建目录中的两个 ESP32 专用翻译单元。它读取共享
源码，插入 port 内的 API、事件及后端实现，不回写共享文件，也不复制维护
GATT/安全实现。所有插入位置必须唯一；上游源码结构变化时会明确报错。
输入文件列入 `CMAKE_CONFIGURE_DEPENDS`，修改后重新生成，随后参与正常
qstr、模块和 GC 根指针提取。

## 兼容行为

- 原有 `gap_advertise()` 固定使用实例 0、传统 PDU、1M PHY，广播和扫描
  响应各最多 31 字节。非连接广播仍可扫描，与原来的 GAP 配置一致。
  广播间隔向下取整到 625µs；旧接口的零刻度间隔仍使用 NimBLE 默认值。
- `adv_data=None` / `resp_data=None` 复用该实例上次的数据；空缓冲区清空
  对应数据，包括空 `bytearray`。缓存只在控制器接受对应数据后更新；
  后续启动失败不会撤销已接受的数据。保存的数据由 GC 根指针保护，关闭协议栈后释放引用。
  控制器复位也会清除缓存状态，避免沿用已经失效的广播/周期配置。
- `gap_advertise(None)` 只停止实例 0，不会停止新接口创建的实例。
- 旧 `gap_scan()` 使用 1M PHY，只向旧事件传递传统 PDU 报告，保持
  `_IRQ_SCAN_RESULT` 的五项元组与 `_IRQ_SCAN_DONE` 不变。
- 超过扩展 HCI 单次扫描时长上限的扫描分段执行，只在最终结束时报告
  扫描完成；时长向上取整到 10ms，取消扫描会清除剩余时间。启停与分段
  续扫由 GIL 串行化，回调按请求代次校验；分段间隙也可以取消，旧完成
  事件不会操作随后启动的新扫描。
- 扩展扫描的间隔和窗口使用扩展 HCI 的 16 位上限（最多
  `40,959,375` 微秒），传统 `gap_scan()` 仍使用传统上限；扩展扫描的窗口
  不能大于间隔，越界参数返回 `EINVAL`。
- 原有 `gap_connect()` 默认使用 1M；GATT、配对和安全事件保持原路径。
- 新事件复用共享同步 IRQ 实现的 GIL、异常保护和 memoryview 生命周期。
  地址与数据的 memoryview 只在回调期间有效，保存时必须复制。
- NimBLE 主机任务中的 IRQ 直接调用 `active(False)` 会抛出
  `OSError(EBUSY)`，协议栈不会开始关闭。IRQ 应设置标志，由主循环或
  asyncio 任务执行关闭；主机任务不能同步等待自身退出。`active(True)`
  会重启协议栈，因此在主机任务 IRQ 中同样返回 `EBUSY`；`active()` 查询不受影响。
- 连接与周期同步使用相同的 GAP 分发入口，避免 SDK 自动重试共享回调
  字段时互相覆盖；普通连接的自动重试配置和原有连接/GATT/安全 IRQ 保留。

原有 aioble 程序仍使用上述旧接口。项目根目录新增的 `micropython-lib` submodule
中，aioble 已适配这些 BLE5 API：`scan(..., extended=True, phys=...)`、
`advertise(..., extended=True)`、`Device.connect(phys=...)`、连接 PHY 查询/更新、
周期广播上下文和周期同步报告迭代器。使用方法见
[`aioble/README.md`](../../micropython-lib/micropython/bluetooth/aioble/README.md#esp32-ble-5-extensions)。
需要安装该 submodule 中的适配版或通过其 manifest 冻结；工作流的默认固件没有冻结
aioble，安装上游原版也不会包含这些扩展。新增事件使用 aioble 自身的 IRQ 分发机制，
应用不能另行调用 `ble.irq()` 覆盖它。控制器仍只有一个扫描过程；aioble 的新旧扫描
统一管理，广播实例 0 与新实例独立，传统与扩展可连接广播不能同时等待连接。

## 新接口

PHY 常量：`PHY_1M=1`、`PHY_2M=2`、`PHY_CODED=3`。
位掩码常量：`PHY_1M_MASK=1`、`PHY_2M_MASK=2`、`PHY_CODED_MASK=4`。

```python
import bluetooth

ble = bluetooth.BLE()
ble.active(True)
print(ble.ble5_features())

# 设置以后连接的默认 PHY 偏好；也可把 None 换成已有 conn_handle。
ble.gap_set_phy(None, bluetooth.PHY_1M_MASK | bluetooth.PHY_2M_MASK,
               bluetooth.PHY_1M_MASK | bluetooth.PHY_2M_MASK)
# ble.gap_phy(conn_handle) -> (tx_phy, rx_phy)

# 可连接的扩展广播。扫描响应只能用于 scannable=True 的广播。
ble.gap_advertise_ext(100_000, adv_data=b"\x02\x01\x06",
                     instance=1, secondary_phy=bluetooth.PHY_2M)
ble.gap_advertise_ext(None, instance=1)
ble.gap_advertise_ext_stop(1, remove=True)

# 同时扫描 1M/Coded；2M 不用于主广播信道扫描。
ble.gap_scan_ext(10_000, phys=bluetooth.PHY_1M_MASK | bluetooth.PHY_CODED_MASK)
ble.gap_scan_ext(None)

# 用指定 PHY 建立连接；取消连接继续使用 gap_connect(None)。
# ble.gap_connect_ext(addr_type, addr, 2000, phys=bluetooth.PHY_CODED_MASK)

# 周期广播要求先配置不可连接、不可扫描的扩展广播实例。
ble.gap_advertise_ext(100_000, adv_data=b"\x02\x01\x06",
                     instance=1, connectable=False, scannable=False, sid=1)
ble.gap_periodic_advertise(100_000, b"\x03\xff\x01\x02", instance=1)
ble.gap_periodic_advertise(None, instance=1)
ble.gap_advertise_ext_stop(1, remove=True)

# 周期同步：地址/SID 从扩展扫描报告获得，期间需保持扫描运行。
# ble.gap_periodic_sync(addr_type, addr, sid=1, skip=0, timeout_ms=10000)
# ble.gap_periodic_sync(None)          # 取消待建立的同步
# ble.gap_periodic_sync_stop(handle)   # 终止已建立的同步
```

`gap_set_phy(conn_handle, tx_phys, rx_phys, *, coded=0)`：PHY 参数为非零
位掩码；`coded=0/1/2` 分别表示无偏好、S2、S8。默认 PHY 设置不接受
S2/S8 参数。设置异步生效，结果由 `IRQ_PHY_UPDATE` 上报。

`gap_advertise_ext(interval_us, adv_data=None, *, resp_data=None, instance=1,
connectable=True, scannable=False, primary_phy=1, secondary_phy=1, sid=0,
timeout_ms=0)`：实例必须大于 0；主 PHY 仅允许 1M/Coded，辅助 PHY
允许 1M/2M/Coded。可连接和可扫描不能同时为真。可扫描扩展广播的数据放在
`resp_data` 中，不能同时设置非空 `adv_data`。非扫描广播不能设置非空
`resp_data`。`timeout_ms=0` 表示无限；其他值向上取整到 10ms。
修改仍在运行的周期广播实例会返回忙错误，须先停止周期广播。

`gap_advertise_ext_stop(instance=1, *, remove=False)`：同时停止该实例的
周期广播和扩展广播。`remove=True` 删除实例配置和缓存数据。

`gap_scan_ext(duration_ms=0, *, interval_us=1280000, window_us=11250,
active=False, phys=1)`：`None` 停止，0 无限。扩展扫描对所有报告发送新
事件；传统 PDU 另外发送旧扫描事件。数据报告可能分片，新事件携带
`data_status`，由使用者重组，不会把不完整数据误标为完整广播。

`gap_connect_ext(addr_type, addr, scan_duration_ms=2000, *,
min_conn_interval_us=0, max_conn_interval_us=0, phys=1)`：仍使用旧连接事件。

`gap_periodic_advertise(interval_us, adv_data=None, *, instance=1)`：
间隔向下取整到 1.25ms 单位，最小 7.5ms；`None` 停止，省略数据保留控制器中的数据。
运行中再次调用会返回忙错误，修改间隔或数据前须先停止周期广播；空缓冲区清空数据。
`gap_periodic_sync(addr_type, addr=None, *, sid=0, skip=0, timeout_ms=10000)`：
`skip` 范围 0..499，超时 100..163840ms，向下取整到 10ms。
`addr_type` 为 0（公共地址）或 1（随机地址）；显式 `addr` 必须为 6 字节。
省略 `addr` 或传入 `None` 时使用控制器的周期广播者列表，该模式下控制器
忽略 `addr_type`/`sid`。列表必须预先通过 NimBLE 原生接口配置；当前 Python
API 没有添加、删除或清空该列表的方法。`gap_periodic_sync(None)` 取消待建立的同步。
IDF 5.5–6.1 的自动重试会丢失列表模式。因此启用
`CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT` 时，省略地址或 `addr=None`
返回 `EOPNOTSUPP`。此外，IDF v5.5.0–v5.5.3 的 NimBLE 周期同步重试会破坏 SDK
内部状态；该版本组合下即使使用显式地址，周期同步也返回 `EOPNOTSUPP`。此限制不
影响普通连接的自动重试；IDF v5.5.4 及以上的显式地址同步不受 F8 限制。需要列表
模式时须在固件配置中关闭自动重试，再预先配置广播者列表；此时普通连接也不再使用
SDK 自动重试。

## 新 IRQ 元组

| 常量 | 编号 | data |
| --- | --- | --- |
| `IRQ_PHY_UPDATE` | 40 | `(conn_handle, status, tx_phy, rx_phy)` |
| `IRQ_SCAN_RESULT_EXT` | 41 | `(addr_type, properties, primary_phy, secondary_phy, sid, periodic_interval, data_status, addr, rssi, tx_power, adv_data)` |
| `IRQ_ADV_COMPLETE_EXT` | 42 | `(instance, reason, conn_handle)` |
| `IRQ_PERIODIC_SYNC` | 43 | `(status, sync_handle, sid, periodic_interval, phy, addr_type, addr)` |
| `IRQ_PERIODIC_REPORT` | 44 | `(sync_handle, data_status, rssi, tx_power, adv_data)` |
| `IRQ_PERIODIC_SYNC_LOST` | 45 | `(sync_handle, reason)` |

`periodic_interval` 是协议中的 1.25ms 单位；RSSI/发射功率单位 dBm，127
表示未知。扫描 `properties` 位：可连接 1、可扫描 2、定向 4、扫描响应 8、
传统 PDU 16。扫描 `data_status`：完整 0、还有片段 1、截断 2。
周期报告还可能为接收失败 3。
新事件的 status/reason 保留 NimBLE 原始值；普通方法失败仍返回现有
Bluetooth 绑定风格的 `OSError`。广播完成仅在 reason=0 时有有效的
连接句柄，其他情况用 65535；周期同步失败时其余字段为零。

## 验证边界

按照项目要求，本次不执行 ESP-IDF 构建，也不声称已通过实机验证。
已检查：

- `make_bindings.py` 的生成结果、幂等性、共享源码不变、模块及 GC 根指针
  注册唯一性、上游结构变化时拒绝生成，以及六类芯片的 CMake 选择条件。
- 基于 IDF 5.5、5.5.1、5.5.2、5.5.3、5.5.4、5.5.5、6.0、6.0.1、6.1
  对应 NimBLE 提交的真实头文件，本轮完成 162 项 C 语法和类型检查，
  包括新增 API/后端/事件、生成的关闭入口和 ESP32 主机任务代码；检查同时验证
  F8 的版本与自动重试限制分支。
  覆盖仅 PHY、扩展广播、周期广播、周期增强和自动重试开关。
- Python 脚本的 Ruff 检查、格式检查和 `git diff --check`。

上述类型检查使用 SDK 的真实 GAP 原型和结构，MicroPython 运行时使用
声明替身，不能代替实际固件构建、链接或控制器互操作测试。
当前 MicroPython 推荐 ESP-IDF 5.5.5；本次 BLE5 工作流和审计目标为 IDF
5.5 及以上，不代表已经完成实际固件构建。

复查生成流程：

```powershell
& D:\conda\envs\hass\python.exe -B ports/esp32/ble5/test_bindings.py
```

复查 SDK 接口：`ble5/check_sdk.py --headers <NimBLE host 头文件目录>
--hci-header <nimble/hci_common.h> --compiler <C 编译器> --idf-version <版本>`。
版本须与对应 SDK 头文件匹配。可重复提供
`--headers`，脚本依赖 `pcpp`、`tree-sitter`、`tree-sitter-c`。
检查使用 MSVC `/Zs` 或 GCC/Clang `-fsyntax-only`，不生成目标文件。

## 手动构建固件

GitHub Actions 中的 `Build ESP32 BLE 5 firmware` 工作流可通过
`Run workflow` 手动触发，使用 ESP-IDF 5.5.5 分别构建 S3、C2、C3、C5、
C6、H2 的通用板固件，另构建 `ESP32_GENERIC_S3` 的 `SPIRAM_OCT` 变体。
实际构建配置必须启用 BLE 5 各功能，并且生成
ESP32 专用 Bluetooth 绑定，才会上传产物；不构建原始 ESP32、S2、P4。
每种芯片分别上传 `micropython-ble5-<芯片>` artifact，保留 14 天，包含
`factory.bin` 合并镜像、`micropython.bin` 应用镜像、实际 `sdkconfig`、
SHA256 校验值和烧录说明。`factory.bin` 从地址 `0x0` 烧录；这些镜像
采用对应通用板的默认 Flash/PSRAM 配置。S3 的 `SPIRAM_OCT` 构建使用
该变体的 Octal PSRAM 配置，单独上传 `micropython-ble5-esp32s3-SPIRAM_OCT`
artifact；默认 S3 构建仍上传 `micropython-ble5-esp32s3`。

后续需要在有原生 BLE 5 控制器的实机上验证：传统 aioble 广播/扫描/连接/
GATT/配对；关闭再开启协议栈；广播数据复用和空数据清除；传统与扩展实例
并行；长时间扫描与取消；2M/Coded PHY 协商；超过 31 字节的扩展广播及
分片报告；周期广播发现、同步、接收、超时和终止。再分别确认原始 ESP32、
S2、P4 的构建未引入 BLE 5 API。C2 尤其需要确认新增控制器内存占用。
