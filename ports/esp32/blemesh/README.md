# ESP32 BLE Mesh：第一阶段

此分支新增可选的原生 `blemesh` 模块和 `aioble.mesh`，在同一 NimBLE Host、
同一控制器上与已有 BLE5 共存。默认固件不启用 Mesh，已有 `bluetooth.BLE`
方法、IRQ 编号/数据格式、aioble 默认导出保持不变。

**当前提供 C3/C5/C6/H2/S3 的 Mesh + BLE5 变体，PB-ADV 节点。** C3/C6/H2/S3 接受
ESP-IDF 5.5.x（≥5.5.3）或 6.1.x；C5 要求 5.5.x ≥5.5.5 或 6.1.x，统一推荐 5.5.5。已完成主机回归和
基于本地 SDK 头文件的静态检查；按本仓库要求，没有编译/链接 ESP32 固件，
也没有进行实机配网、互通、功耗或射频共存验证。下述构建命令供实机验证时使用。

## 功能范围

- 一个节点、一个主元素：Configuration Server、Health Server、一个 Vendor Model。
- PB-ADV 配网；由外部 Provisioner 完成 NetKey/AppKey、模型绑定和组订阅配置。
- Vendor 收发；两个三字节 opcode，低层使用 `ESP_BLE_MESH_MODEL_OP_3(0/1, company_id)`。
- NVS 保存 Mesh 配置；停止、重启默认保留配网信息；提供显式清除接口。
- 独立事件队列及异步接收，不占用 `bluetooth.BLE.irq()`。

本阶段不提供 PB-GATT、Proxy、Provisioner、Relay、Friend、Low Power Node、
Mesh 1.1 扩展、任意动态模型或 SIG 应用模型。Mesh 使用传统 Mesh 广播 PDU；
SDK 的 BLE5 API 只用于分配独立广播实例，**不会把 Mesh 网络改成 Coded/2M 网络**。

配网端必须支持 **PB-ADV**。只支持 PB-GATT 的手机应用无法直接为本阶段固件配网；
可以使用支持 PB-ADV 的开发板 Provisioner 或其他工具。

## 构建与安装

在已配置好上述 ESP-IDF 版本的环境中，从仓库根目录选择对应板卡构建：

```sh
make -C ports/esp32 BOARD=ESP32_GENERIC_C3 BOARD_VARIANT=BLEMESH
make -C ports/esp32 BOARD=ESP32_GENERIC_C5 BOARD_VARIANT=BLEMESH
make -C ports/esp32 BOARD=ESP32_GENERIC_C6 BOARD_VARIANT=BLEMESH
make -C ports/esp32 BOARD=ESP32_GENERIC_H2 BOARD_VARIANT=BLEMESH
make -C ports/esp32 BOARD=ESP32_GENERIC_S3 BOARD_VARIANT=BLEMESH
# S3 八线 PSRAM 板使用这个变体：
make -C ports/esp32 BOARD=ESP32_GENERIC_S3 BOARD_VARIANT=BLEMESH_SPIRAM_OCT
```

现有手动工作流 `.github/workflows/build_esp32_ble5.yml` 已包含上述六个 Mesh 组合，
与纯 BLE5 固件分别生成产物，使用 IDF 5.5.5。本轮未触发 CI 构建。

`BLEMESH` 变体在现有 BLE5 配置之后叠加 `boards/sdkconfig.blemesh`。
必须使用该变体自己的构建目录；若旧的生成 `sdkconfig` 覆盖了默认值，应清理该
变体的构建目录后重新配置。CMake 会拒绝不支持的芯片/SDK、错误实例数及冲突选项。
SDK 原文件不修改：配置阶段在构建目录生成 Mesh adapter 和广播 worker，并替换 bt 目标内对应源文件。
SDK 锚点变化时直接报错，不能无检查地升级 SDK。

版本检查接受 5.5.3 起的 5.5.x 和 6.1.x；已核对的标签为 5.5.3、5.5.4、5.5.5、6.1。
5.5～5.5.2 的 Mesh adapter 缓存结构不同，本阶段不适配。5.5.3 仍有既有的 NimBLE
周期同步重试缺陷：启用连接自动重试时，本分支拒绝周期同步请求；周期广播不受这条
限制影响。优先使用 5.5.5。版本放行不代表未来补丁版已经验证，源码漂移检查仍生效。

### 芯片范围

| 系列 | 本阶段 Mesh + BLE5 共存 |
| --- | --- |
| C3、C6、H2、S3 | 提供 `BLEMESH`；5.5.3 起的 5.5.x 或 6.1.x，均待目标构建和实机验收 |
| C5 | 提供 `BLEMESH`；5.5.5 起的 5.5.x 或 6.1.x，均待目标构建和实机验收 |
| C2 | 保留纯 BLE5；上述 SDK 均设 `SOC_BLE_MESH_SUPPORTED=0`，未提供 Mesh 变体 |
| 原始 ESP32 | 支持 BLE4.2/Mesh 的其他方案，但不满足本分支 BLE5 共存条件 |
| S2、P4 | 无片内蓝牙控制器；外接控制器方案不在本阶段范围内 |

共存并非 C6 独占。C5 在 5.5.3/5.5.4 尚未声明 SDK Mesh 能力，5.5.5/6.1 才声明；
C2 的 BLE5 能力不能替代缺失的 SDK Mesh 支持。要扩展 C2，需要单独移植/验证 SDK，
不能仅强行开启布尔配置。H2 没有 Wi-Fi，应用中若还依赖 WLAN，需要另外处理。

C3/S3 使用 `BT_CTRL_BLE_MAX_ACT=10`，沿用已有板级配置；这些槽位由广播、扫描、
连接和周期同步共享，不能同时用满五个广播实例和九条连接。C5/C6/H2 在 NimBLE 模式
从 Host 配置取得控制器广播容量，不要求 C3/S3 不存在的 `BT_LE_*` 配置。

默认固件只包含原生模块。使用异步包装时，可以将本地
`micropython-lib/micropython/bluetooth/aioble/aioble/mesh.py` 安装到设备
`/lib/aioble/mesh.py`，并安装本分支 aioble 的依赖。也可在自定义冻结 manifest 中加入：

```python
include("$(PORT_DIR)/boards/manifest.py")
require("aioble")
require("aioble-mesh")
```

构建时通过 `FROZEN_MANIFEST=/path/to/manifest.py` 选择它，使用本工作区的
`micropython-lib`。`aioble-mesh` 是新增的本地可选包，不能假设上游 mip 已发布它。
`from aioble.mesh import Node` 显式加载扩展；普通 `import aioble` 不依赖原生 Mesh。
注意 `micropython-lib` 是独立 Git 仓库，需要单独保存其新增文件。

Mesh 会增加 Flash/RAM 使用量。本阶段没有测量固件大小/堆余量，未调整现有分区表、
BLE 连接数量和堆配置；首次构建须检查 app 分区是否足够及启动后的可用内存，必要时
为 BLEMESH 变体配置更大的 app 分区。不要据此推断原分支的最大连接数能在 Mesh
扫描和广播同时运行时满负荷使用。

## 原生使用

```python
import bluetooth
import blemesh

ble = bluetooth.BLE()
ble.active(True)

# 每个物理节点必须有稳定且唯一的 16 字节 UUID。
# 下例身份仅用于隔离实验；产品使用自己的 Company ID 和唯一 UUID。
uuid = bytes.fromhex("1234567890abcdef1234567890abcdef")
blemesh.start(uuid, company_id=0x1234, model_id=1)

# start 返回表示 SDK 初始化完成；新节点等待 READY 后进行 PB-ADV 配网。
# 配网后，Provisioner 仍需添加 AppKey 并绑定到上述 Vendor Model。
print(blemesh.status())
event = blemesh.recv()  # 无事件时返回 None

# 完成 AppKey 绑定后发送，索引/目标地址必须与实际网络配置一致：
# blemesh.send(0x0002, b"hello", net_idx=0, app_idx=0, opcode=0)

# 停止 Mesh，不停止 BLE5 广播、连接或应用扫描，也不擦除配网状态。
# blemesh.stop()
```

同一个节点重新启动时必须使用相同 UUID、Company ID 和 Model ID。
本阶段不会自动迁移 NVS 中保存的 Composition；更换身份/模型前，先在原配置运行
状态调用 `blemesh.reset()` 清除配网数据，再启动新配置并重新配网。
Provisioner 中旧节点记录也应相应清理。远端 Configuration Node Reset 产生 `RESET`
事件后，可 `stop()` / `start()` 重新开启配网。

### API

| 调用 | 行为 |
| --- | --- |
| `start(uuid, *, company_id, model_id=1)` | BLE 必须已 active；单例，重复启动报 `EBUSY`；UUID 长度 16 |
| `stop()` | 停止并保留 NVS；停止状态重复调用无操作 |
| `reset()` | 仅运行时允许；停止并擦除 Mesh 配置，不擦除整个 NVS |
| `status()` | 返回 `state/provisioned/address/dropped/error/radio_error` |
| `irq(handler)` | VM 上下文调用 `handler(None)`，只是通知有数据；`None` 解除处理器 |
| `recv()` | 非阻塞取出一个事件，空队列返回 `None` |
| `send(dst, data, *, net_idx, app_idx, opcode=0, ttl=7)` | 发送 Vendor 消息；opcode 只能为 0 或 1；数据最多 192 字节 |

`state`：0 停止、1 启动中、2 运行、3 停止中、4 失败。`error` 是最近的 SDK
初始化/配网使能/停止错误，或导致 Mesh 进入 FAILED 的广播/Host 错误；
`radio_error` 仍只表示扫描恢复错误，不能作为 Mesh 整体健康状态。
私有广播被抢占、异常终止或 Host reset 时，发送等待以错误结束，产生 `ERROR`，
`status()["state"]` 返回 4，后续发送被拒绝。请显式 `stop()` 后再以原身份 `start()`；
不会自动重试或把异常广播完成改报为成功。重新启动保留 NVS，消息重发由应用决定。
此前已交付的 `SENT` 无法撤回；它仍不是空口交付或远端 ACK，节点级 `ERROR` 也不与
某个 Vendor 消息一一对应，需要可靠交付时由应用使用事务号和确认机制。
`ERROR` 也受事件队列容量限制，应同时检查 `status()`；普通 API 错误仍通过异常返回。
无法安全清理的初始化/停止失败会保持状态 4，须 `machine.reset()`；不会尝试启动第二套 Host。
若只是关闭前排空在途操作超时，SDK 任务尚未销毁，可重试 `stop()`；SDK deinit 本身
失败则不能重试进入已销毁的工作线程，须硬件复位。
软复位时若无法安全停止 Mesh，将执行硬件重启，保留 NVS。

事件是固定八元组：

```text
(event, src, dst, net_idx, app_idx, opcode, payload, status)
```

| 事件常量 | 说明 |
| --- | --- |
| `READY` (1) | 已恢复节点或 PB-ADV 配网已使能，不代表 AppKey 已绑定 |
| `PROVISIONED` (2) | 配网完成；src 是分配的主地址，net_idx 是网络索引 |
| `RESET` (3) | 收到 SDK 节点重置通知 |
| `MESSAGE` (4) | Vendor 消息；src/dst 是消息源/接收目标；opcode 是完整的三字节编码值 |
| `SENT` (5) | 本地发送完成；status 为 SDK 错误码，**不是远端 ACK** |
| `STOPPED` (6) | 本地停止完成 |
| `ERROR` (7) | 异步错误；超长接收数据会产生 `E2BIG` |
| `CONFIG` (8) | 配置服务器状态改变；opcode 是配置操作码，不包含密钥 |

其他未使用字段为零或空 bytes。网络/应用密钥由 SDK 管理，不回传 Python。
队列固定 16 个事件，满时丢弃新事件并递增 `dropped`；启动时清零。
IRQ 通知可以合并，处理时必须循环 `recv()` 直到 `None`，或定期轮询。
收取事件分配 Python 对象失败时该事件也可能丢失；本接口不提供消息可靠交付保证。
`send` 支持单播和组地址，不支持需要 Label UUID 的虚拟地址；SDK 负责分段、加密与重传。
应用级确认、去重和事务超时应在 Vendor 协议中自行定义。

## aioble.mesh

```python
import asyncio
import blemesh
from aioble.mesh import Node

async def main():
    uuid = bytes.fromhex("1234567890abcdef1234567890abcdef")
    async with Node(uuid, company_id=0x1234) as node:
        async for event in node:
            if event[0] == blemesh.MESSAGE:
                print("from", event[1], "data", event[6])
            else:
                print(event)

asyncio.run(main())
```

`Node.start()` 自动激活已有 BLE 单例；`stop()` 只停止 Mesh。
`node.send(...)` 和 `node.status()` 对应原生接口。
`await node.recv(timeout_ms=5000)` 支持超时与任务取消，每个节点只允许一个接收任务。
停止会唤醒接收者并抛出 `MeshStoppedError`，异步迭代正常结束；溢出抛出
`MeshOverflowError`，之后可以继续接收。`aioble.stop()` 会随 BLE 生命周期一起停止 Mesh。
同一 Node 可以立即 stop/start：旧接收者仍以 `MeshStoppedError` 结束，新会话可立即
开始接收；取消旧任务不会释放新会话的接收所有权。原生状态进入 FAILED 时，待接收
任务也会被唤醒并以 `MeshStoppedError` 结束，通过 `node.status()` 查询具体错误。
不要在 Node 拥有原生模块期间直接替换 `blemesh.irq()` 或调用原生 `recv/stop/reset`。
生命周期方法在主循环/asyncio 任务中使用，不要在 BLE Host 的同步 IRQ 中调用。

## BLE5 共存与自动 lightsleep

- 两种固件均设置 `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=4`，即总共 **5** 个实例。
  纯 BLE5 的公共句柄为 **0～4**，能力查询返回 5；Mesh 固件的公共句柄为 **0～3**，
  能力查询返回 4，物理实例 **4** 为 Mesh 私有。原有句柄 0～2 保持兼容。
  SDK Host 的 Kconfig 上限为 4，C5/C6/H2 控制器的对应配置上限也为 4，不能直接
  设为 5 来获取六个实例；C3/S3 还受共享控制器活动槽位限制。
  Mesh 固件中依赖句柄 4 的纯 BLE5 新代码需要依据能力查询减少并发广播。
  SDK 的 Mesh role ID 仍为 3（其 Kconfig 上限），生成 adapter 在启动/停止边界映射到
  物理实例 4。完成事件交给带会话标识的私有信号量，不再从 Host 回调通知 SDK 任务
  句柄；SDK 广播 worker 获取实际完成错误后传给原有发送结束回调。
  停止先禁止新广播启动并排空在途操作，再销毁 SDK 任务，迟到的旧会话事件被丢弃。
  Mesh 启停不重新注册应用 GATT 服务。
- BLE5 与 Mesh 分别订阅共享扫描器；取消/超时只移除自己的订阅，事件按各自 PHY
  转发。被动扫描的应用不接收另一方主动扫描产生的扫描响应。
- 两方扫描同时运行时合并参数：每种 PHY 选择更短的 interval、连续 window，任一
  主动扫描请求优先。应用的超时时间和事件格式保持，但实际扫描占空比、收包数量、
  功耗会变化。Mesh 使用数据副本，应用仍获得原始广播字节。
- 发起 BLE 连接时暂停物理扫描，完成/失败/取消后恢复订阅；这段时间 Mesh 可能漏包。
  已有连接、扩展/周期广播继续共享射频时隙。不能保证零丢包或与单独 BLE5 相同吞吐。
- Mesh 不增加永久全局 PM 锁，复用现有自动 lightsleep 的唤醒路径。然而普通 Mesh 节点
  通常需要持续扫描，实际休眠机会和功耗由控制器决定。**自动 lightsleep 不等于 LPN**。

### Mesh 固件能否也保留应用的 1+4 个实例？

这需要 **6 个物理实例**：应用 0～4 加 Mesh 私有实例 5，超过当前官方 Kconfig 的
配置范围；该范围不等于已证明的硬件极限。可研究单独的 ESP-IDF/controller 补丁方案：
放宽 Host/控制器配置范围，核对实例数组、控制器初始化参数及库实现、内存和活动数，
确认 HCI `LE Read Number of Supported Advertising Sets` 返回至少 6，再做六实例并发、
周期广播和 Mesh 配网实测。仅修改 Kconfig 上限不能证明控制器能够正常工作。

本阶段没有修改外部 SDK，也没有实现 5+1。当前配置保留应用 0～3 和 Mesh 私有 4；
不要将 Mesh 与应用复用同一物理实例，这会破坏独立停止、连接及周期广播的语义。
如后续验证通过，可将公共容量改为 5、私有实例改为 5，并调整 adapter/CMake 检查。

## 验证

从仓库根目录运行，Python 主机检查不生成 ESP32 固件：

```sh
python ports/esp32/ble5/test_bindings.py
python micropython-lib/micropython/bluetooth/aioble/tests/test_ble5.py
python micropython-lib/micropython/bluetooth/aioble/tests/test_mesh.py
python ports/esp32/blemesh/tests/test_adapter.py --idf /path/to/esp-idf
python ports/esp32/blemesh/tests/test_cmake.py --idf /path/to/esp-idf --idf-version 5.5.5 --cmake cmake
python ports/esp32/blemesh/tests/test_scan.py --compiler cc
python ports/esp32/blemesh/tests/test_advertising.py --idf /path/to/esp-idf --compiler cc
python ports/esp32/blemesh/check_sdk.py --idf /path/to/esp-idf --compiler cc
```

Windows 的扫描主机测试需在 VS 开发者命令行使用 `--compiler cl`。
模拟 CMake 测试需要 Ninja，可用 `--ninja /path/to/ninja` 指定；仅检查源文件替换和配置
拦截，不运行 ESP-IDF/Kconfig 或固件构建。
SDK 类型检查依赖 `pcpp/tree-sitter/tree-sitter-c`，同 BLE5 静态检查脚本。
`ble5/check_sdk.py` 新增 `--mesh`，用于检查 Mesh 开启时的 BLE5 编译分支。

`tests/coexistence_smoke.py` 是手动实机冒烟脚本，检查公共 API、GATT 本地句柄、
扫描超时及重复启停，需在专用测试板上使用。它不能代替第二台设备的空口观察。
正式验收还需外部 Provisioner 和 BLE5 对端：

1. BLE5 普通/扩展/周期广播运行时完成 PB-ADV 配网、AppKey 绑定和 Vendor 双向收发。
2. 保持已建立的 GATT 连接，反复启停 Mesh，确认服务句柄、读写/通知不变。
3. Mesh 消息持续收发时启动/取消/超时普通及 Coded 扫描，观察 Mesh 持续可用。
4. 同时测试连接成功、失败、取消、周期同步创建/丢失/重建，确认扫描恢复。
5. 重启恢复配网，显式 reset 重新配网；软复位和 BLE active(False/True) 不留下旧回调。
6. 验证队列溢出、长消息、NVS 空间不足、低内存、控制器异常及自动 lightsleep 唤醒。
7. Mesh 连续发包期间配对/绑定（触发 resolving-list 抢占），以及注入 Host reset：
   确认 `ERROR`/FAILED 可观测、没有伪成功，stop/start 后恢复；清理失败则硬件复位。
8. 正在广播时反复 stop/reset/BLE 关闭并立即重启，确认无空任务通知及旧会话串扰。

详细决策与验证边界见根目录 [BLEMesh设计审计.md](../../../BLEMesh设计审计.md)。
