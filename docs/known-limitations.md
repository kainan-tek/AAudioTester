# 实现层已知取舍（Developer-facing Known Limitations）

本文记录通知与恢复协议中**设计上接受的取舍**——不是待修 bug 清单。每条 = 限制 + 为什么接受；替代方案若有必要也一并说明，避免后续审查重复推演。

约定：

- 只记录"刻意接受"，不记录"待修"；新问题先经审查确认性质再归入本文。
- 改动相关机制时同步更新对应条目。
- 只引用机制名与文件名，不引用行号（行号会腐烂）。

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

AAudio 数据/错误回调只设标志位；错误消息经 `RtErrorSlot`（栈上 buf + `publishf`）由 worker 线程投递 JNI。约束：回调线程上不能调用任何 JNI 函数，消息组合只能走 `publishf`（`vsnprintf`，每条死流至多触发一次）。

为什么接受：回调线程违反 RT 约束会引入音频毛刺与死锁风险；消息体都是短静态/格式化文本，栈上组合足够。

## 4. C++ 异常不得穿越 JNI 边界

`std::thread` 构造失败（EAGAIN）或堆分配失败等 C++ 异常若离开 JNI 函数，结果是 `std::terminate`（进程终止），不会变成 Kotlin 异常。native 代码未对此做防御。

为什么接受：这类失败意味着 OS 级资源耗尽，进程终止即崩溃点==原因点，与 Kotlin 侧 `System.loadLibrary` 的 fail-fast 原则一致；为它们建立跨边界的错误通道成本远超收益。

## 5. 配置拒绝只报错、不回滚 Spinner

native 拒绝 `setNativeConfig`（唯一可达路径：`GetStringUTFChars` OOM）时，引擎经 `onError` 报 `[PARAM] Native rejected configuration`，但不通知 Fragment 回滚 Spinner 的选中项——UI 继续显示被拒配置，直到用户重新选择任意配置项（触发新的 `setAudioConfig` 重新同步）。

为什么接受：完整回滚需要 Fragment 反向读取引擎状态回选 Spinner，增加 UI 耦合；拒绝本身是内存压力级边缘，报错可见已消除"UI 与引擎静默分叉"的主要危害。
