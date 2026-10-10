# ESP32 自动 light sleep

ESP32 port 的接口为 `machine.freq([hz[, light_sleep_enable]])`，仅接受位置参数。
`light_sleep_enable` 可选，省略时为 `False`。

```python
import machine
import time

hz = machine.freq()
machine.freq(hz, True)   # 保持当前运行频率，并允许自动 light sleep
time.sleep_ms(1000)      # 任务阻塞时，IDF 可以选择进入休眠
machine.freq(hz, False)  # 关闭自动 light sleep
machine.freq(hz)         # 旧调用也会关闭自动 light sleep
```

`machine.freq()` 仍返回当前 CPU 频率，不改变休眠开关。开机和软复位后自动休眠
默认关闭；软复位保留频率。关闭时保留原来的短等待方式和 `machine.idle()` 行为。
开启时，运行频率仍固定，未加入动态调频范围。

H2 可设置 16/32/48/64/96 MHz，默认 96 MHz 可以直接用于上述用法。
P4 提供 20/40/90/100/180/200/360/400 MHz 的候选值，具体可用档位由对应 IDF
版本和芯片修订配置校验；默认 360/400 MHz 可用于开启休眠。其他芯片保留原频率范围。

## 关闭自动休眠时的旧脚本兼容性

`machine.freq(hz)` 与 `machine.freq(hz, False)` 都设置固定频率并禁止 CPU 自动
light sleep。旧脚本不调用第二参数时，不会主动开启这项功能；无参 getter 则只读频率，
如果其他代码已启用自动休眠，读取频率不会将其关闭。

关闭状态下，有线程支持的事件等待和 `sleep_ms()` 保留一 tick 等待，无线程构建的
事件等待保留 WFI/WAITI；`machine.idle()` 仍只让出 CPU。新增 scheduler/BLE 唤醒通知
受开关控制，自动模式关闭后不启用长等待。显式 `machine.lightsleep()`/`deepsleep()`
仍可调用：此开关禁止的是自动休眠，不禁止旧脚本主动请求的手动休眠。

普通旧脚本的接口和主要执行语义保持兼容，但不能保证整个固件的时序、功耗和可用内存
与旧版完全相同。H2/P4 的频率白名单已修正，BLE 控制器 modem sleep 的编译配置仍然
有效，且新增 IRAM/USB 状态维护有少量开销。新的最大连接数默认配置还会增加蓝牙内存
需求，即使 CPU 自动休眠关闭，也不会恢复旧蓝牙资源配额；内存接近上限的旧应用需复测，
必要时在板型配置中降低连接数。连接容量与具体配置见 `README.ble5.md`。

如果启动代码或库主动调用过 `machine.freq(hz, True)`，旧脚本可先执行
`machine.freq(machine.freq(), False)` 再运行。该调用关闭自动机制，不撤销 BLE 编译配置，
也不把整个 SDK/驱动状态回滚成旧固件。

## 实际进入休眠的条件

构建需要 `CONFIG_PM_ENABLE=y` 和 `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`。
ESP32 的 `sdkconfig.base` 已启用二者，但已有构建目录的 `sdkconfig` 优先级更高，
需要使用新构建目录或更新旧配置。缺少支持时，启用调用抛出 `ValueError`。
USB Serial/JTAG 的全局休眠检查还需要 `CONFIG_ESP_TIMER_IN_IRAM=y`（已在 base 配置
中启用）；自定义配置关闭该项时，启用自动休眠会抛 `OSError`。全局 USB 回调注册失败
也会返回错误，不会静默启用缺少 USB 保护的自动休眠。

开关只允许休眠。所有 CPU 的任务都空闲、预计空闲时间达到 IDF 阈值，并且没有
驱动持有阻止休眠的 PM 锁时，IDF 才会自动休眠。忙循环、活动的硬件定时器、持续
扫描以及其他线程都可能阻止休眠。无需循环调用 `machine.lightsleep()`。

`time.sleep_ms()` 和 asyncio 的事件等待在开启时可以阻塞多个 tick，每次最多
100 ms。等待仍会响应任务通知和待执行回调；BLE 同步 IRQ 结束后会通知主任务，
因此 aioble 的 `ThreadSafeFlag` 可以及时唤醒事件循环。没有任务通知的自定义
流或其他线程直接设置的标志最多额外等待 100 ms。WebREPL/socket 事件回调存在时
保持每 tick 轮询，普通 `select.poll()` 使用有限的等待窗口，避免无限阻塞网络输入。
`time.sleep_us()` 仍采用原来的精确忙等待。
等待被唤醒后，会先处理调度回调，再让 `poll` 重新检查对象，避免回调设置标志后
又多等待一个 100 ms 窗口。这不构成硬实时延迟保证。

开启后的 `machine.idle()` 处理事件并等待至多 100 ms；关闭时仍只让出 CPU。
REPL 等待输入保持每 tick 轮询，因此在 REPL 提示符处通常不会进入自动休眠。

## BLE、USB 与 UART

PM/tickless 配置独立于 BLE5，经典 ESP32、S2 和 P4 也可使用自动休眠接口。

BLE 控制器休眠配置由 CMake 分别选择：S3/C3 使用 `sdkconfig.ble_sleep_c3`，
C2/C5/H2 使用 `sdkconfig.ble_sleep`，C6 额外使用 `sdkconfig.ble_sleep_c6` 满足
MAC/BB retention 要求。经典 ESP32 和 P4 保留各自原来的控制器配置。

启用自动休眠前会注册全局 USB skip-sleep 回调，覆盖 socket 接收、DNS、锁等待等
不经过 HAL 的阻塞路径。回调只调用 IRAM 访问器读取 DRAM 状态，不调用 TinyUSB。
该回调在一次开机内只注册一次，跨软复位保留，自动休眠关闭时不会强制进入睡眠。

USB Serial/JTAG 在最近 100 ms 内有 SOF 或刚初始化时阻止睡眠；超时后允许休眠。
USB CDC/TinyUSB 从总线 reset/setup/resume 事件开始阻止睡眠，明确 unplug 事件后释放。
主机 suspend 仍保留保护；没有可靠 VBUS/拔线事件的板型可能继续阻止休眠，测量低功耗时
应断开 USB 后冷启动。这样不会将无法区分的主机挂起误判为可以断开连接。

UART REPL 在自动休眠启停时设置/撤销 UART 唤醒。唤醒所用的前导字符会丢失，不能把
第一条完整命令或单个 Ctrl-C 当作可靠唤醒序列。主机应先发送几次 `a` 等含足够 RX 边沿的
前导字符，留出唤醒时间后再发送换行/实际命令，并容许清理残留字符。P4 的阈值为 6 个
正边沿，其他目标为 3；具体间隔及发送流程需按硬件验证。

## 与手动休眠共存

自动休眠开启时，`machine.lightsleep()`、`machine.sleep()` 和 `machine.deepsleep()`
抛出 `ValueError`，避免手动路径清除 BLE/Wi-Fi 注册的唤醒源。应先关闭自动休眠，
再使用手动休眠接口。手动休眠期间保持无线连接不在该接口的保证范围内；之后如需
恢复自动休眠与无线通信，应重新初始化无线驱动。

```python
machine.freq(machine.freq(), False)
machine.lightsleep(1000)
```

## 验证

`tests/ports/esp32/machine_auto_lightsleep.py` 验证可选参数、手动休眠保护、调度回调、
等待截止时间，以及旧调用和显式关闭的兼容性，可通过项目的 `tests/run-tests.py`
在 ESP32 设备上运行。该用例不代表已经测得低功耗；需另行断开 USB 测量电流，
验证 BLE5 广播、连接、UART 唤醒，以及 S3 Octal-SPIRAM 的重复休眠和软复位。
默认频率被拒绝现在视为测试失败；仅旧固件不支持第二参数，或明确缺少 PM/tickless
支持时报告 SKIP。`machine_auto_lightsleep_schedule.py` 另覆盖等待中调度回调并设置流
就绪的情况；验证长等待延迟时应关闭 USB 活动及 socket event 回调造成的短轮询。

宿主静态检查脚本为 `ports/esp32/check_lightsleep.py`，依赖与 BLE5 检查脚本相同：

```text
python ports/esp32/check_lightsleep.py --idf <IDF目录> --idf-tag v5.5.5 --tinyusb <TinyUSB源码目录> --compiler <cl或gcc或clang>
```

TinyUSB 源码需对应 lockfile 中的提交，目录下包含 `src/device/dcd.h`。该脚本只执行
C 语法/类型检查，不构建固件，不验证射频、USB 实际枚举或控制器睡眠恢复。
