# 实现层已知取舍（Developer-facing Known Limitations）

本文记录**设计上接受的取舍**与评估后**拒绝的审查意见**——不是待修 bug 清单。取舍条 = 限制 + 为什么接受；拒绝条 = 建议 + 拒绝理由；替代方案若有必要也一并说明，避免后续审查重复推演。

约定：

- 只记录"刻意接受"，不记录"待修"；新问题先经审查确认性质再归入本文。
- 改动相关机制时同步更新对应条目。
- 只引用机制名与文件名，不引用行号（行号会腐烂）；commit hash 与测试名是稳定锚点，允许引用。
- 条目后缀按性质区分：「（审查意见，评估后拒绝）」= 改进建议被否；「（审查发现，评估后不修）」= 审查发现的真实瑕疵被评估接受；取舍条不加后缀。

README 的「已知限制」小节记录的是**用户可见的产品级限制**；本文记录**实现层的可靠性取舍**，受众是开发者与代码审查者。

---

## 1. 错误通知投递失败的降级策略

跨语言通知链路：native 一次性 latch（`JavaNotifier`，aaudio_common.h）→ Kotlin 监听器回调。投递失败（线程 attach 失败、`NewStringUTF` OOM、监听器抛出）时：

- 错误文本丢失，只留在 logcat（`AAC_LOGW`）。
- **UI 恢复优先于错误保真**：播放器 EOF 路径立即降级为 stopped 通知（reader 线程内已完成 teardown，可自动收尾）；录音器因 stream reset 只允许在 executor 线程（writer 线程不能碰 stream），恢复必然经过 `stopNative` 的 stopped 通知——先前丢失的错误在用户侧被降级为"正常停止"。
- 投递失败期间 UI 短暂显示 active，直到用户手动停止（或到达上述降级通知）。
- 此策略同样约束 `stopNative` 时的收尾报告（`[FILE]` finalize 失败 / `[TRUNC]` 录音不完整），有两种丢失方式：报告投递失败而后续停止通知投递成功时，坏头或不完整的文件在用户侧表现为一次正常完成的录音（文件完好性只在 logcat 可辨）；或一次性 latch 已被更早的会话级错误通知认领，收尾报告被跳过——此时用户已收到"会话异常结束"的错误通知，丢失的只是次要细节（如流死亡之外另有尾部数据未存盘）。

为什么接受：投递失败本身是内存压力级别的罕见边缘；恢复路径（手动 Stop → `stopNative`）始终可用；文件与会话状态保持一致（录音文件完整包含死流前的全部音频，丢失量在时长上一目了然）。

为什么替代方案更糟：

- **不释放 latch**：`stopNative` 的 `deliverStoppedOnce` 会认为"已通知"而跳过投递，UI 永久卡死在 active，连手动恢复都失效。
- **worker 线程重试投递**：失败原因（attach 失败、监听器抛出）是持续性的，重试即自旋；向已证明会抛出的监听器重试也违反下一条策略。
- **把消息放回 `RtErrorSlot` / 持久化丢失的错误**：`take()` 的一次性 exchange 是为防 publish/take 竞态而设计的，放回旧消息重新引入该竞态；单独的"丢失错误"槽位是为退化边缘新增协议状态。

## 2. 抛出型监听器按"未投递"处理

监听器回调抛出异常时，该通知计为**未投递**：异常在 native 侧清除（绝不穿越 JNI 边界、绝不随 `DetachCurrentThread` 悬挂），latch 释放，恢复走第 1 条的降级路径。

为什么接受：向已证明会抛出的监听器重试只会重复失败；异常的清除点与判定点在同一处，语义单一。

## 3. RT 回调线程不触 JNI、不做堆分配

AAudio 数据/错误回调只设标志位；错误消息经 `RtErrorSlot`（静态字面量指针的原子交接，无 buf、无 RT 线程格式化）由 worker 线程投递 JNI。约束：回调线程上不能调用任何 JNI 函数，`publish` 只能传静态存储的文本（具体错误原因由 errorCallback 的 LOGE 行打在相邻日志）。

为什么接受：回调线程违反 RT 约束会引入音频毛刺与死锁风险；消息体都是短静态文本，指针交接足够。

## 4. C++ 异常不得穿越 JNI 边界

`std::thread` 构造失败（EAGAIN）或堆分配失败等 C++ 异常若离开 JNI 函数，结果是 `std::terminate`（进程终止），不会变成 Kotlin 异常。native 代码未对此做防御。

为什么接受：这类失败意味着 OS 级资源耗尽，进程终止即崩溃点==原因点，与 Kotlin 侧 `System.loadLibrary` 的 fail-fast 原则一致；为它们建立跨边界的错误通道成本远超收益。

## 5. 配置拒绝只报错、不回滚 Spinner

native 拒绝 `setNativeConfig`（唯一可达路径：`GetStringUTFChars` OOM）时，引擎经 `onError` 报 `[PARAM] Native rejected configuration`，但不通知 Fragment 回滚 Spinner 的选中项——UI 继续显示被拒配置，直到用户重新选择任意配置项（触发新的 `setAudioConfig` 重新同步）。

为什么接受：完整回滚需要 Fragment 反向读取引擎状态回选 Spinner，增加 UI 耦合；拒绝本身是内存压力级边缘，报错可见已消除"UI 与引擎静默分叉"的主要危害。

## 6. 跨引擎互斥是时序性的，不是原子保证

播放/录音互斥的实现：切 Tab 必然触发旧 Fragment 的 `onPause`，向**本 section 的 executor** 提交异步 `stop()`（MainActivity 的 ViewPager2 + AAudioTestFragment 的 onPause 钩子）。两个 section executor 相互独立并行，旧引擎的 stop 与新引擎的 start 之间没有任何协调点——若旧引擎的 stop 排在长任务之后（慢存储上的线程 join、release 收尾等），新引擎的 `startNative` 可能在旧引擎真正停流之前执行，物理上出现一个短暂的"同时播放+录音"重叠。

特征：

- 窗口通常毫秒级：`stopNative` 第一行即清 `is_playing`/`is_recording`，数据回调下一个周期即返回 STOP；重叠只在"切 Tab 后极快点 Start"的操作序列下可能出现。
- 仅物理层重叠，UI 无卡死路径：旧引擎的 `onStopped` 通知最终一致地刷新按钮（`isAdded` 已守卫）；长时间双活跃在 UI 层不可能发生（每个 Tab 的 Start 只被本引擎的 isActive 门控，但切 Tab 必触发 onPause stop）。
- README 手动验证清单第 3 条「无法同时播放+录音」描述的是常规操作与 UI 语义；本条目澄清其物理保证强度。

为什么接受：两引擎用独立 executor 是刻意设计（一个引擎的慢 teardown 不得推迟另一个引擎的 start，见 AAudioTestFragment 的 sectionExecutors 注释），跨引擎强互斥必然要打破这一隔离或引入新的共享状态。

为什么替代方案更糟：

- **合并为单 executor**：直接违背上述隔离设计，慢存储场景下一个引擎的收尾会卡住另一个引擎的启动。
- **共享互斥标志（如跨引擎 AtomicBoolean gate）**：需要为"start 遇对方 active"定义失败语义（报错？等待重试？），并处理 gate 与两端状态机之间的新竞态面——复杂度与一个毫秒级窗口的收益不成比例。
- **切 Tab 时同步等待旧引擎停止**：阻塞主线程，或引入跨线程握手协议。

---

以下条目记录 2026-09 代码审查中**评估后拒绝**的修改建议——建议本身成立但收益不抵成本，存档避免后续审查重复推演。

## 7. 播放器路径二次 resolve：`applyNativeConfig` 与 `beforeStartNative` 各调一次（审查意见，评估后拒绝）

有审查意见建议消除 `AAudioPlayer.resolvePath` 的重复调用：`applyNativeConfig`（config 变更路径）与 `beforeStartNative`（start 前置校验路径）对同一 config 各 resolve 一次。

拒绝理由：`resolvePath` 是确定性纯函数（ifBlank 折叠 + asset 展开 + 异常回退原路径），同 config 两次调用结果必然一致，重复调用只有微小的解析开销、无正确性影响。消除它必须引入缓存——而缓存在 config 每次变更时都要失效同步，把"无状态重复"换成"有状态缓存"是净负收益；且 `beforeStartNative` 的 `.wav` 校验对象必须是**实际将传给 native 的最终路径**，与 `applyNativeConfig` 共享结果反而把两条职责链耦在一起。

## 8. `determineFocusType` 用字符串 contains 匹配（审查意见，评估后拒绝）

有审查意见建议把 `AAudioPlayer.determineFocusType` 的 `usage.contains("NAVIGATION")` / `contains("VOICE_COMMUNICATION")` 改为对常量 key 的精确匹配，或在 `AAudioConstants` 映射表里为每个 usage 预存焦点类型。

拒绝理由：usage 名不是自由文本——它全部来自 `AAudioConstants` 的受控映射表 key（Spinner 取值与配置文件枚举都受其约束），contains 在受控词表上语义等价于枚举匹配，不存在误匹配路径。改映射表存结构化焦点类型需要动 MAP 定义、全部 getter 与相关测试，是零功能收益的纯风格重构。

## 9. 系统 usage 魔数 1000 与 `audioFocusRequest!!`（审查意见，评估后拒绝）

有审查意见建议具名化 `AAudioPlayer.requestAudioFocus` 的 `>= 1000` 系统 usage 判定（如定义 `SYSTEM_USAGE_BASE = 1000`），并消除 `audioFocusRequest!!` 的非空断言。

拒绝理由：1000 不是随意的魔数——`AudioAttributes.USAGE_EMERGENCY` 等系统 usage 常量是 `@hide`（SDK 不可见），`AAudioConstants` 映射表里本就只能写字面量 1000-1003（该处有注释锚定），播放器的 `>= 1000` 是同一 SDK 约束的直接延续，具名化只是把字面量换个地方放，零功能收益。`audioFocusRequest!!` 的上一行刚完成赋值、不可能为 null，改局部变量纯属风格；该字段其余访问点均用安全调用，`!!` 只此一处，可读性影响有限。

## 10. XML 配置解析不加固 XXE（审查意见，评估后拒绝）

有审查意见建议对 `AAudioConfig.parseConfigs` 做 XXE 加固（`DocumentBuilderFactory` 逐 feature `setFeature` 禁用 DOCTYPE 与外部实体，不支持时降级）。

拒绝理由：采姊妹项目 AudioTester known-limitations §8 的同一判断。威胁模型上：配置源为 root 可写的本地路径（外部文件优先、否则 assets），能放置恶意配置的前提是已持有设备 root，而 root 本就拥有设备与 app 的一切——加固不改变威胁模型。失败模式上：带 DOCTYPE 的合法配置被拒后经 `loadConfigs` 静默落回应急配置，对拿着失效配置做测试的人是陷阱——日志告警与单测锁定只能缓解、不能消除。测试工具里配置的正确生效优先于防御纵深。

历史注记：2026-09 审查曾实施该加固（逐 feature try-catch 降级 + DOCTYPE 拒绝测试），复评时确认姊妹项目已有关键决策、且其理由在本项目同样成立，遂整体回撤——本条目即该反复的最终结论，后续审查不必再推演。
