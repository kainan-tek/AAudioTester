# AAudioPlayer 延迟测试功能

## 功能说明

AAudioPlayer 集成了音频延迟测试功能，通过 GPIO 电平翻转和音频数据变化的同步来测量音频链路延迟。

## 工作原理

1. 每次音频回调写入数据时，计数器递增
2. 每写入 100 次数据，GPIO 电平翻转一次
3. 同时音频在静音和正常播放间切换
4. 用示波器监测 GPIO 和音频 TDM 口，测量时间差

## 配置参数

```cpp
#define LATENCY_TEST_ENABLE 0                              // 启用/禁用功能 (默认禁用)
#define LATENCY_TEST_GPIO_FILE "/sys/class/gpio/gpio376/value"  // GPIO 文件路径
#define LATENCY_TEST_INTERVAL 100                          // 翻转间隔（次数）
```

## 使用方法

### 1. 编译配置

- 设置 `LATENCY_TEST_ENABLE 1` 启用功能
- 设置 `LATENCY_TEST_ENABLE 0` 禁用功能

### 2. 硬件连接

- 示波器通道1：连接 GPIO376
- 示波器通道2：连接音频 TDM 数据线

### 3. 测量延迟

观察 GPIO 电平变化和音频数据变化的时间差即为音频延迟。

## 性能优化

- **预打开文件描述符**：避免回调中重复打开/关闭 GPIO 文件
- **直接系统调用**：使用 `write()` 替代 C++ 流操作
- **减少日志输出**：仅每 100,000 次写入输出状态日志
- **内联函数**：GPIO 操作函数标记为 `inline`

## 错误处理

当 GPIO 初始化失败时：

- 自动禁用测试功能
- 不影响正常音频播放
- 输出错误日志提示

## 注意事项

1. 需要 GPIO 访问权限
2. GPIO376 需配置为输出模式
3. 启用功能会增加少量 CPU 开销
4. 测试失败不影响音频播放功能