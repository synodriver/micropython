# ESP32 自动 light sleep

ESP32 port 的接口支持读取、旧式固定频率设置和三位置参数的 PM 配置：
`machine.freq()`、`machine.freq(hz)`、`machine.freq(min_freq_hz, max_freq_hz, light_sleep_enable)`。
频率参数以 Hz 为单位；ESP-IDF PM 配置使用 MHz，三参数形式的频率输入必须是正的整 MHz。
两参数形式不支持。

```python
import machine
import time

hz = machine.freq()
machine.freq(80000000, 240000000, True)  # 在 80–240 MHz 范围内调频，并允许自动 light sleep
time.sleep_ms(1000)      # 任务阻塞时，IDF 可以选择进入休眠
machine.freq(hz, hz, False)  # 固定在当前频率，关闭自动 light sleep
machine.freq(hz)             # 旧式单参数调用仍设置固定频率并关闭自动休眠
```

示例中的 80/240 MHz 适用于支持这两个频点的芯片；各 ESP32 芯片支持的频点和组合不同，
IDF 会校验实际范围。范围模式下 `machine.freq()` 返回查询时的实际 CPU 频率，可能在范围内变化。
`machine.freq()` 读取频率时不改变休眠开关。开机和软复位后自动休眠
默认关闭；软复位保留频率。关闭时保留原来的短等待方式和 `machine.idle()` 行为。
开启时可由 PM 在配置的最小和最大频率之间动态调频。

H2 可设置 16/32/48/64/96 MHz，默认 96 MHz 可以直接用于上述用法。
P4 提供 20/40/90/100/180/200/360/400 MHz 的候选值，具体可用档位由对应 IDF
版本和芯片修订配置校验；默认 360/400 MHz 可用于开启休眠。其他芯片保留原频率范围。

## 关闭自动休眠时的旧脚本兼容性

`machine.freq(hz)` 与 `machine.freq(hz, hz, False)` 都设置固定频率并禁止 CPU 自动
light sleep。旧脚本只调用单参数 setter 时，不会主动开启这项功能；无参 getter 则只读频率，
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

如果启动代码或库主动启用了自动休眠，旧脚本可先执行
`machine.freq(machine.freq())` 再运行。该调用关闭自动机制，不撤销 BLE 编译配置，
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

可以用 `print(esp32.pm_dump_locks())` 查看当前 PM 锁的名称、类型、参数及 `Active`
计数；返回值是字符串，便于保存或自行分析。`ESP_PM_NO_LIGHT_SLEEP` 类型的锁在
`Active` 非零时阻止自动 light sleep。此接口无需启用 `CONFIG_PM_PROFILING`，因此
默认不提供累计持有次数或持有时间；未启用 `CONFIG_PM_ENABLE` 时抛 `OSError`。
锁计数可能在输出过程中变化；忙任务、USB skip-sleep 回调等非 PM 锁原因不会列在
这里，不能仅凭所有 `Active` 为零就断言设备必然进入休眠。

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

## Python PM 锁：临时阻止自动休眠

`esp32.PMLock(kind, *, name=None)` 创建一个初始未获取的锁；它独立于 BLE5，
不要求 `CONFIG_PM_PROFILING`。`CONFIG_PM_ENABLE` 关闭时创建抛
`OSError(EOPNOTSUPP)`。三种类型常量位于 `esp32` 模块：

| 常量 | 约束 |
| --- | --- |
| `PM_NO_LIGHT_SLEEP` | 持有期间禁止 PM 自动 light sleep |
| `PM_CPU_FREQ_MAX` | 要求使用 PM 配置的最高 CPU 频率，也阻止自动休眠 |
| `PM_APB_FREQ_MAX` | 要求 APB 最大频率模式，实际频率由芯片及 PM 配置决定，也阻止自动休眠 |

旧式 `machine.freq(hz)` 将 PM 最小/最大 CPU 频率设为同一值；三参数范围形式可分别设置。
若只需阻止自动休眠，通常选择 `PM_NO_LIGHT_SLEEP` 即可。CPU 锁不把频率提升到芯片硬件上限，也不锁定某个数值；
锁存续期间仍可以调用 `machine.freq()` 修改配置。

```python
import esp32
import machine
import time

lock = esp32.PMLock(esp32.PM_NO_LIGHT_SLEEP, name="sensor")
try:
    hz = machine.freq()
    machine.freq(hz, hz, True)
    with lock:
        time.sleep_ms(100)  # 此期间禁止自动 light sleep
        print(esp32.pm_dump_locks())
    # 锁已释放，但仍存在，可重复使用；满足其他条件后可以自动休眠。
finally:
    lock.deinit()
```

- `acquire()` 和 `release()` 返回 `None`，不等待其他调用者，支持嵌套计数；
  获取 N 次须释放 N 次。未持有时释放抛 `RuntimeError`。这是电源约束计数，
  不是线程互斥锁；多个模块可分别创建对象，各自维护自己的获取/释放。
- `with lock as value` 返回同一个对象，进入获取一次、退出释放一次；异常不被吞掉。
  协程可在普通 `with` 内 `await`，取消导致作用域退出时也会释放。不要在作用域内
  手动释放由 `with` 获取的那一次计数。
- `deinit()` 返回 `None`，可重复调用；持有期间调用抛 `OSError(EBUSY)`。
  销毁后 `acquire()`、`release()` 和再次进入 `with` 抛 `ValueError`。
- `name` 只能为 `str` 或 `None`，不允许内嵌 NUL；名称被复制并与锁对象一起存储。
  保存最多 15 个 UTF-8 字节的完整字符前缀，避免 SDK 截断多字节字符导致诊断返回
  `UnicodeError`；长名称可能显示为相同前缀。
- GC 最终回收对象时释放剩余计数并删除 SDK 锁；软复位在线程停止后通过现有
  `gc_sweep_all()` 清理全部对象，包括仍有 Python 引用的锁。正常代码应使用 `with`
  或 `try/finally`，GC 的时机不保证，任务异常终止也不会自动释放仍被引用的锁。
  不要手动调用用于 GC 的 `__del__()`。
- 普通 Python 线程调用在 GIL 内串行化，普通协程/调度回调可调用；不支持硬中断，
  ISR 中调用抛 `RuntimeError`。持锁不保证 Python IRQ 回调的硬实时响应。

创建、释放和销毁均不修改自动休眠开关或唤醒源。关闭自动休眠后，释放锁不会
重新开启休眠。PM 锁也不阻止应用显式调用 `machine.lightsleep()` / `deepsleep()`；
原有“自动模式开启时禁止手动休眠”的检查继续有效。无线驱动的 PM 锁独立管理，
Python 只能操作自己创建的锁。最后一个用户锁释放后仍不保证立即休眠。

设备测试：`tests/ports/esp32/esp32_pm_lock.py` 检查嵌套、异常、名称及 GC；
`esp32_pm_lock_async.py` 检查协程取消。软复位应另在 REPL 创建并获取命名锁，
按 Ctrl-D 后确认 `pm_dump_locks()` 中已无该名称。实际功耗、时序仍需实机验证。
静态检查脚本 `check_pm_lock.py --idf <IDF目录> --idf-tag v5.5.5 --compiler <编译器>`
使用真实 MicroPython 绑定宏和 IDF PM 头文件，不生成目标文件或固件。

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
hz = machine.freq()
machine.freq(hz, hz, False)
machine.lightsleep(1000)
```

## GPIO 与 Pin.irq 唤醒

### 普通 Pin.irq：运行期间和自动休眠期间共用 handler

启用自动 light sleep 后，普通 `Pin.irq(handler=..., trigger=...)` 自动参与 GPIO
唤醒，不需要再配置 `wake_on_ext*()`，也不要加 `wake=`。先注册 IRQ 再启用自动模式，
或先启用再注册，均可；`pin.irq(handler=None)` 注销并取消该 IRQ 的自动唤醒。
`pin.irq().trigger()` 返回用户设置的触发类型，setter 同样经过唤醒兼容性检查。

| trigger | 自动模式下的行为 |
| --- | --- |
| `IRQ_RISING` / `IRQ_FALLING` / 二者按位或 | 运行时保留硬件边沿检测；入睡前暂时改成与当前电平相反的 GPIO 电平唤醒，醒后比较电平，并按订阅方向通知 |
| `IRQ_LOW_LEVEL` / `IRQ_HIGH_LEVEL` | 每段**观察到的**有效电平通知一次，随后监测恢复无效电平，再允许下一次通知；注册时已有效也会通知 |

电平常量也是现有 `WAKE_LOW` / `WAKE_HIGH` 的别名。持续按住按钮不会反复交付
电平 callback；松开可产生一次内部唤醒，但只订阅按下方向的 handler 不会因此调用。
这是一种电平有效区间通知语义，**不同于关闭自动休眠时原有的持续电平重复中断**。
关闭自动模式后恢复用户的硬件 trigger。单参数 `machine.freq(hz)` 仍关闭自动模式。

handler 保持 `handler(pin)` 签名，在 MicroPython 调度上下文中运行；硬件 ISR 和
PM 回调只记录/调度事件，不执行 Python。使用静态调度节点，普通 Python 调度队列满
时不会直接丢弃已记录的通知。同一引脚在交付前的多次事件合并，包括运行期间的事件。
注销或重新配置 IRQ 会丢弃该引脚尚未交付的旧通知；异常由受保护的回调调用处理。

例如 H2 上的空闲 GPIO8，按钮按下接地、使用外部上拉，和 asyncio 配合：

```python
import asyncio
import machine
from machine import Pin

button = Pin(8, Pin.IN)  # H2 示例；确认板上未占用，并连接外部上拉
changed = asyncio.ThreadSafeFlag()

def button_irq(pin):
    changed.set()

async def main():
    hz = machine.freq()
    button.irq(handler=button_irq, trigger=Pin.IRQ_LOW_LEVEL)
    try:
        machine.freq(hz, hz, True)
        while True:
            await changed.wait()
            print("按钮有效电平通知，当前电平:", button.value())
            # 此处可以 await；应用按需要增加消抖与业务处理。
    finally:
        button.irq(handler=None)
        machine.freq(hz)

asyncio.run(main())
```

这里没有为了检测按下而进行的应用层定时轮询。`ThreadSafeFlag` 本身也合并通知；
不能将通知次数当作物理按键次数或脉冲计数。callback 不是协程，不能直接 `await`，
需要通过 flag 把处理交给协程。`time.sleep()` 等待中也可处理 callback，但不会因此
提前结束原定延时；底层仍保留最长约 100 ms 的等待窗口，不代表设备只被按钮唤醒。

**限制与配置冲突：**

- 第一版仅使用数字 GPIO light-sleep 唤醒，要求数字 GPIO 电源域保留、
  `CONFIG_PM_LIGHT_SLEEP_CALLBACKS=y`。关闭回调或开启
  `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` 的构建，在存在普通 IRQ 时启用
  自动模式，或在自动模式中注册 IRQ，会抛 `OSError(EOPNOTSUPP)`；没有自动回退 EXT1。
- 引脚必须始终是有效的专用数字输入，电平稳定且满足硬件唤醒采样要求。不要与
  Flash/PSRAM、USB、ADC、Touch、ULP、外设复用或应用自己的保持配置共用。注册后
  不要用 `Pin.init()`、直接寄存器访问或其他驱动改变该引脚的用途。数字 GPIO 唤醒
  不受 EXT0/EXT1 的 RTC 引脚集合限制，但仍须遵守具体芯片的 GPIO 硬件限制。
- 自动模式下，同一引脚的普通 IRQ 与显式 `wake_on_ext0/ext1/gpio()` 睡眠唤醒配置
  冲突会抛 `OSError`；先后两种配置顺序都检查，不覆盖原配置。不同引脚可共存，SDK
  仍可能拒绝其他组合，例如经典 ESP32 的 GPIO 唤醒与 Touch/ULP 唤醒。
- **不能保证捕获每个边沿或任意窄脉冲。** 睡前为高、醒后为低只表示至少发生过一次
  下降变化；睡眠或交接期间若高→低→高，最终电平相同，可能完全不通知，即使短暂电平
  曾唤醒 CPU。没有根据旧的 wake reason 伪造事件，也不从一次 GPIO 唤醒推断脉冲次数。
- 回调可能合并；硬件 ISR 与恢复过程的交接、真实抖动也可能造成重复通知，不保证
  每个物理事件恰好一次。电平释放后又迅速有效且释放未被观察到，也可能被合并。
  此功能不提供消抖、精确计数、事件时间戳或硬实时响应；需要这些保证时应保持运行，
  配合硬件计数/锁存功能。解释器长期阻塞时，callback 也会延迟。
- 自动模式关闭时继续使用原普通 IRQ 调度路径，其有限 Python 队列仍可能溢出。
  手动 light sleep 不使用这个适配机制；deep sleep 重启后不保留 handler。

### 显式配置睡眠唤醒源

`esp32.wake_on_ext0()`、`wake_on_ext1()`、`wake_on_gpio()` 的配置同时适用于手动休眠
和自动 light sleep。先配置再调用 `machine.freq(hz, hz, True)`，或者在自动模式开启后
更新配置，均可生效。关闭自动模式时撤销它安装的唤醒源，保留 Python 侧配置供下次
启用或手动休眠使用；软复位会清空这些配置。动态更新失败会恢复旧配置；如果恢复也
失败，会关闭自动模式并抛异常，避免在缺少预期唤醒源时继续休眠。

例如 ESP32-H2 使用空闲 GPIO8，外部下拉，拉高唤醒：

```python
import esp32
import machine
import time
from machine import Pin

p = Pin(8, Pin.IN)  # 此例用于 H2；请确认板上引脚空闲并连接外部下拉
esp32.wake_on_ext1(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
hz = machine.freq()
machine.freq(hz, hz, True)
time.sleep_ms(1000)
esp32.wake_on_ext1(pins=[])  # 取消 EXT1 唤醒
hz = machine.freq()
machine.freq(hz, hz, False)
```

显式 GPIO 唤醒可以提前唤醒 CPU；`time.sleep_ms()` 仍等待原定截止时间，不会因 GPIO 唤醒
直接提前结束，也不会自动产生 Python 回调。低电平唤醒使用 `WAKEUP_ALL_LOW` 常量，
其中经典 ESP32 的 EXT1 表示所有所选引脚都低，其他支持 EXT1 的目标表示任一所选
引脚低。GPIO 路径的低电平条件同样是任一所选引脚低。

- `Pin.irq(trigger=Pin.WAKE_LOW/WAKE_HIGH, wake=machine.SLEEP)` 保留 EXT0 语义，
  限经典 ESP32、S2、S3 的有效 RTC 引脚；加入 `machine.DEEPSLEEP` 可同时配置深睡唤醒。
  只指定 `DEEPSLEEP` 不会安装为自动 light-sleep 唤醒源。
- H2/C2/C3/C5/C6/P4 的 `Pin.irq(wake=...)` 明确抛 `ValueError`。有 EXT1 的目标使用
  `esp32.wake_on_ext1()`；C2/C3 使用 `wake_on_gpio()`。deep sleep 仅允许相应 SDK
  支持的引脚。H2 的 EXT1 外部唤醒可用 GPIO8～14，GPIO7 未引出用于外部唤醒。
- `IRQ_RISING/IRQ_FALLING` 与非 `None` 的 `wake` 参数组合仍会报错。
  `Pin.irq` 的显式 `wake=` 分支保留原有 EXT0 语义，不注册传入的 handler；
  上述普通 IRQ 自动唤醒使用的是不传 `wake=` 的分支。
- GPIO 唤醒共用中断类型寄存器。同一引脚混用在自动模式下已明确拒绝；手动休眠
  仍不提供普通 IRQ 兼容保证。撤销显式 GPIO 唤醒时，只在中断类型仍等于本功能
  设置的值时恢复旧值，避免覆盖应用后来设置的不同类型。不要依赖捕获任意窄脉冲。
  撤销、关闭或安装失败时，也恢复安装前的 GPIO 休眠输入/输出及 sleep selection 状态。
- EXT0/EXT1 醒后恢复数字引脚复用并释放睡眠保持；唤醒引脚应作为专用数字输入使用，
  不与 ADC、Touch、ULP、其他外设或应用自己的保持配置共用。同一引脚同时配置
  RTC 唤醒和 GPIO 唤醒会在应用配置时被拒绝。
- EXT0 用 `pin=None` 取消；EXT1/GPIO 用 `pins=[]` 取消，`pins=None` 保持原引脚集合。
  配置和进入睡眠时的 SDK 错误会抛异常；已满足唤醒电平或手动休眠时间过短则正常返回。
  deep sleep 醒后重新启动，原 Python 对象和 IRQ handler 不恢复。

自动 EXT0/EXT1 恢复需要 `CONFIG_PM_LIGHT_SLEEP_CALLBACKS=y`，已加入 `sdkconfig.base`；
已有构建目录需更新最终 sdkconfig。关闭此项的自定义构建仍可自动休眠和使用 GPIO 唤醒，
但配置自动 EXT 唤醒会抛 `OSError`。开启 `PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP`
的自定义构建不能使用这里的普通 GPIO light-sleep 唤醒路径，应使用有效 EXT1 引脚。
临时 PM 锁仅保护配置更新，不根据 IRQ 注册情况阻止日常自动休眠。

`wake_on_ulp(True)` 的自动模式仅支持 FSM ULP。IDF 5.5～6.1 的逐源关闭接口不能完整
撤销 RISC-V/LP-core ULP 唤醒，自动模式下这类配置抛 `OSError(EOPNOTSUPP)`；先配置
再开启自动模式也会被拒绝。手动休眠仍支持这些 ULP 类型，并在返回或安装失败后清除
本次 SDK 唤醒源。自动路径不会因此调用 `ESP_SLEEP_WAKEUP_ALL` 清除无线驱动配置。

## 验证

`tests/ports/esp32/machine_auto_lightsleep_irq.py` 检查普通调度队列满时静态 IRQ 通知、
事件合并、注销待处理事件、注册顺序、显式唤醒冲突回滚、trigger getter/setter 和
asyncio flag。该用例用软件调用 IRQ 对象，不验证物理睡眠唤醒；选择的输入引脚需空闲。
实机还需分别测试运行/睡眠时外部按下、长按和松开、双边沿、多引脚、短脉冲、抖动、
快速重配、重复启停及软复位，并断开 USB 测电流。不能用该用例通过替代这些验证。

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

新增 GPIO/EXT 路径的静态检查：

```text
python ports/esp32/check_sleep_wakeup.py --idf <IDF目录> --idf-tag v5.5 --compiler <cl或gcc或clang>
```

`tests/ports/esp32/machine_sleep_wakeup.py` 覆盖参数拒绝、启用前后配置、冲突回滚及
deep-sleep-only 配置隔离；运行前确认脚本选择的引脚未被板上外设占用。该测试不证明
实际电流或物理唤醒成功。板上还需验证外部电平触发、取消/更换引脚后旧引脚不再唤醒、
重复休眠后的数字输入、软复位清理，以及 BLE 连接期间 GPIO 唤醒。deep sleep 需单独
验证重启原因和启动脚本，不能用 light-sleep 回归替代。

`machine_sleep_gpio_restore.py` 在 ESP32-S3 空闲 GPIO4 上检查取消唤醒、关闭自动模式
及手动休眠返回后的休眠输入/输出和 selection 恢复。它会临时修改该引脚的休眠寄存器
并在退出时恢复，执行前需确认 GPIO4 未用于板上外设；尚未在设备上运行。
