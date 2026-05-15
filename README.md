# 宠语者 (PetWhisperer)

基于 ESP32-P4 的智能宠物箱监控系统。利用硬件 H.264 编码器的运动矢量（MV）实现零额外开销的运动感知，自动触发录像并存储至 SD 卡，通过 LVGL + MIPI DSI LCD 实时预览。

> **当前状态**: AI 蛇检测三态状态机已上线（MONITOR→AI_VERIFY→RECORDING），活动追踪 JSON 日志写入 SD 卡，YOLO11n 模型骨架就绪待接入 ESP-DL。视频录制暂时搁置。

## 硬件

| 模块 | 型号 | 接口 |
|------|------|------|
| 主控 | ESP32-P4 (PSRAM 32MB) | — |
| WiFi | ESP32-C6 (SDIO 从芯片) | SDIO 4-bit 40MHz |
| 摄像头 | OV5647 | MIPI CSI 2-lane, RAW8→ISP→YUV420 800×800 |
| 显示屏 | EK79007 | MIPI DSI 1024×600, LVGL + PPA 缩放 |
| 触摸 | GT911 | I2C |
| 存储 | MicroSD 32GB SDHC | MMC 4-bit 40MHz, FatFS |
| 音频 | ES8311 Codec | I2S（预留，未启用） |

## 软件架构

```
摄像头 (CSI DMA, uncached)
  │
  ▼
camera_frame_cb()              输入帧回调 (core 1)
  │
  ├── PPA DMA copy             uncached → cached PSRAM (~10ms)
  │     │                      输出格式 = O_UYY_E_VYY (TRM 行交织 YUV420)
  │     ▼
  │   frame pool (PSRAM)       5×960KB 缓冲环
  │     │
  │     ▼
  │   encode_processor_task    编码处理 (core 1, prio 5)
  │     │
  │     ├── PPA display scale   YUV→RGB565 1024×600 (~59ms)
  │     ├── H.264 编码          esp_h264_enc_single_hw (~15ms)
  │     ├── MV 运动检测          两级分类 (strong_mv)
  │     ├── [新] 三态状态机       MONITOR → AI_VERIFY → RECORDING
  │     │     ├── MONITOR       MV 触发阈值检测
  │     │     ├── AI_VERIFY     snake_detect 每帧推理确认
  │     │     └── RECORDING     双重检测(5s) + 位置追踪(1s)
  │     └── SD 入队             异步写入队列 (暂未开录像)
  │           │
  │           ▼
  │         async_sd_writer_task  SD 写入 (core 1, prio 2)
  │
  └── snake_detect              AI 蛇检测 (PPA 800→320 RGB, 骨架)
        │
        ▼
      activity_tracker           活动追踪 (JSONL 写入 SD)
```

### 状态机

```
MONITOR ──MV trigger(strong>200 x5)──▶ AI_VERIFY
                                           │
                                 AI confirm(连续2帧置信度>0.5)
                                           │
                                           ▼
                                       RECORDING
                                    (不录像, 只追踪)
                                           │
                               ┌───────────┴───────────┐
                         每 5s 双重检测          每 1s 记录位置
                         (MV + AI)              (activity_tracker)
                               │
                         两路都失败 10s → MONITOR
```

### 核心组件

| 组件 | 功能 |
|------|------|
| `main/main.c` | 主程序：三态状态机、编码 pipeline、MV 运动检测、双重检测、帧率统计 |
| `components/snake_detect/` | [新] AI 蛇检测 — PPA 800→320 RGB 缩放 + YOLO11n 接口骨架 |
| `components/activity_tracker/` | [新] 活动追踪 — 事件/位置记录、JSONL 日志写入 SD 卡 |
| `components/storage_manager/` | SD 卡管理、文件预分配、MP4 封装 |
| `components/app_video/` | V4L2 摄像头抽象层 |
| `components/esp_muxer/` | MP4 多路复用器 |
| `components/wifi_manager/` | WiFi 配网（SoftAP + HTTP）+ NTP 时间同步 |
| `components/cloud_interface/` | 云端接口（预留） |
| `components/motion_detector/` | 运动检测器（预留） |
| `components/gmf_video/` | 官方 ESP-GMF-Video v0.8.3（分析用，未启用） |

> **关键发现 (2026-05)**: PPA 输出的 YUV420 格式按 TRM 定义为行交织 UYY/VYY，即 O_UYY_E_VYY 格式，与 H.264 硬件编码器输入格式一致。PPA 输出可直接喂给 H.264，无需中间格式转换（省掉 ESP_IMGFX ~20ms）。此前颜色异常（绿色画面）根因为 PPA 输出被误当作标准 I420 喂给 IMGFX 做二次转换。

## 当前功能

- [x] **实时预览**: 摄像头 YUV420 @800×800，PPA 缩放至 1024×600 LCD，每 1 帧显示一次
- [x] **三态状态机**: MONITOR (MV 监控) → AI_VERIFY (AI 确认, 每帧推理) → RECORDING (双重检测 + 位置追踪)
- [x] **AI 蛇检测**: snake_detect 组件骨架 — PPA 800→320 RGB 缩放就绪，模拟推理返回 (ESP-DL 真实推理延后)
- [x] **运动检测**: 基于 H.264 MV 的两级分类 —— `mag<5` 丢弃，`mag≥8` 为强运动块
- [x] **双重检测**: RECORDING 态每 5 秒 MV + AI 联合判定，两路都失败 10 秒后停止
- [x] **活动追踪**: 事件开始/结束时间、每秒蛇头位置采样、JSONL 日志写入 SD 卡 (`/sdcard/logs/activity_YYYYMMDD.jsonl`)
- [x] **NTP 时间同步**: WiFi 连接后自动同步 (ntp.aliyun.com)，所有时间戳用 Unix time
- [x] **H.264 编码**: 硬件编码器 800×800 (仅用于 MV 检测, 暂不录像)
- [x] **异步 SD 写入**: 5×1.5MB PSRAM 缓冲队列 (暂未开录像)
- [x] **帧率统计**: 每 30 帧输出逐操作耗时拆解

## 性能数据

| 操作 | 耗时 | 说明 |
|------|------|------|
| PPA DMA copy (uncached→cached) | ~10ms | 硬件 DMA 替代 CPU uncached memcpy (原 84ms)，输出 O_UYY_E_VYY |
| memcpy (帧池→编码 buffer) | ~1ms | 无需格式转换 (PPA 输出 = H.264 输入) |
| H.264 编码 (800×800) | ~28ms | 硬件编码器 |
| PPA 显示缩放 (YUV→RGB565) | ~59ms | 每 3 帧一次，阻塞模式 |
| SD 写入 (P帧) | 1~5ms | 预分配后极快 |
| SD 写入 (I帧) | ~70ms | 数据量大 |

**预期帧率**: ~12fps（每帧都显示，PPA 阻塞模式约 59ms/帧）。

## 遇到的问题

### 1. BSS 段溢出 → 无法链接

`fullclean` 后 `.dram0.bss` 溢出 4.6MB。`async_write_job_t` 内嵌 `pool_buf[1536*1024]` ×3 = 4.5MB 静态 BSS 远超内部 SRAM 容量。

**修复**: `pool_buf` 从嵌入式数组改为 PSRAM 堆分配指针（`heap_caps_aligned_calloc`），语法不变，3×1.5MB 从 .bss 迁移到堆。

### 2. ESP-IDF v5.5.4 链接器缺陷

`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` 触发 `--enable-non-contiguous-regions` flag，导致 `.sbss` 段被错误丢弃，FreeRTOS/中断/日志变量全部链接失败。这是 ESP-IDF v5.5.4 配合 SPIRAM 时的已知缺陷。

**修复**: 修复 BSS 溢出后不再需要该 flag，链接器恢复正常。

### 3. CPU 缓存一致性问题

PSRAM 上 CPU 写入与 DMA 读取之间缺少 `esp_cache_msync`，导致 H.264 编码器 DMA 读到不完整数据，表现为：
- 视频画面上下半部分撕裂
- 颜色异常（UV 分量错位）
- 左右重复画面

**修复**：所有 CPU↔DMA 交互点插入缓存同步：
```
IMGFX 写入前 → M2C (废弃输出缓冲旧缓存)
IMGFX 写入后 → C2M (CPU 数据刷到 PSRAM，编码器 DMA 可见)
H.264 编码后 → M2C (废弃 CPU 缓存，读编码器 DMA 输出)
PPA DMA 写入后 → M2C (废弃 CPU 缓存，读 PPA DMA 输出)
```

### 4. MV 运动检测噪声问题

低照度下摄像头传感器噪声使 H.264 编码器产生大量随机 MV（mag 2~5，分布 1500+/2500 宏块）。原基于 `|x|<2 && |y|<2` 的过滤几乎无效（800×800 下只过滤 1%），平均值+总数阈值方案完全失效。

**修复**: 两级分类 —— `mag<4` 全丢弃，`mag≥6` 才算"强运动块"计入决策。利用噪声幅度天然低于真实运动 2~3 倍的特征。触发条件：连续 8 帧 strong_mv≥80 启动录像，strong_mv<40 持续 10 秒停止录像。

### 5. FAT32 簇分配开销

FAT32 每次分配新簇需读 FAT 表→修改→写回，零碎写入放大 I/O。录像过程中动态申请簇时产生周期性写入尖峰。

**修复**: 
- 文件预分配（`esp_vfs_fat_create_contiguous_file` 64MB）
- 512KB FILE buffer + 512KB muxer 缓存
- `CONFIG_FATFS_IMMEDIATE_FSYNC=n` 禁止每次写入强制同步
- `CONFIG_FATFS_USE_FASTSEEK=y` 快速 seek

### 6. 0KB 录像文件

`muxer_fclose` 中某些异常路径下文件已创建但未写入数据。

**修复**: `fclose` 前始终 `ftruncate(fileno(f), actual_size)`，无数据时截断至 0。

### 7. activity_event_t BSS 溢出 → SDIO 内存池初始化失败

`activity_event_t` 内嵌 `position_samples[3600]` + `zone_track[3600]` = ~43KB 静态 BSS，挤占内部 DRAM (~500KB) 导致 ESP-Hosted SDIO mempool 初始化失败 (`sdio_mempool_create: no mem`)。

**修复**: `g_current_event` 从静态变量改为 PSRAM 堆分配指针 (`heap_caps_calloc`)，~43KB 从 .bss 迁移到外部 PSRAM。

### 8. RECORDING→MONITOR 编码器重建失败

返回 MONITOR 时调用 `switch_state()` 触发编码器 deinit/reinit，但 RECORDING 一直用的 monitor 配置（未切换），重建时内存不足 (`No memory for reference frame`)。

**修复**: RECORDING→MONITOR 直接改 `g_state`，不重建编码器（同配置无需重建）。

## FreeRTOS 内存与任务优化

### 内存布局策略

```
SRAM (内部, 512KB)              PSRAM (外部, 32MB)
├── 栈 (task + ISR)             ├── 帧缓冲池 5×960KB
├── FreeRTOS 内核               ├── LCD 画布 1.2MB
├── 驱动 / 中断 / DMA 描述符     ├── O_UYY_E_VYY 编码缓冲 960KB
├── 日志缓冲区                   ├── H.264 输出缓冲 512KB
├── FatFS 工作缓冲               ├── 异步 SD 写入池 5×1.5MB
├── SDIO mempool                ├── AI RGB 缓冲 320×320×2
└── 其他小对象                   ├── activity_event 43KB (PSRAM heap)
                                └── FatFS 文件缓存 512KB
```

**原则**: 大缓冲放 PSRAM（堆分配），SRAM 留给栈和内核。跨 DMA 的缓冲必须 cache line 对齐。

### 任务设计

| 任务 | Core | 优先级 | 栈 | 职责 |
|------|------|--------|-----|------|
| `encode_proc` | 1 | 5 | 8KB | PPA显示 + H.264 + MV + AI + 状态机 + 双重检测 |
| `sd_writer` | 1 | 2 | 4KB | 从 ready 队列取数据写 SD (暂未开录像) |
| `app_video` (系统) | — | 默认 | — | 摄像头驱动，帧回调 |
| LVGL tick | — | 默认 | — | UI 刷新，每 10ms |

**核心设计**: 编码 pipeline 与 SD 写入解耦，避免 SD 卡写入尖峰（~70ms I 帧）阻塞编码。

### 异步双队列机制

```
帧编码 task                    SD 写入 task
     │                              │
     ├─ H.264 编码完成              │
     ├─ memcpy → pool_buf[idx]     │
     └─ xQueueSend(ready_queue) ──→ xQueueReceive
                                    ├─ muxer_write (PSRAM → FILE buf)
                                    ├─ 必要时 fflush
                                    └─ xQueueSend(free_queue) ──→ 归还 buffer
```

- **free_queue**: 5 个槽位，标记可用 buffer 索引
- **ready_queue**: 已编码待写 buffer 索引
- 生产者-消费者模式，编码阻塞不影响 SD 写入，反之亦然

### 关键 Kconfig

```ini
CONFIG_FREERTOS_HZ=1000              # 1ms tick，减少调度延迟
CONFIG_COMPILER_OPTIMIZATION_PERF=y  # -O2 性能优化
CONFIG_SPIRAM=y                      # 启用 16MB PSRAM
CONFIG_FATFS_IMMEDIATE_FSYNC=n       # 禁止每次 write 强制 sync
CONFIG_FATFS_PER_FILE_CACHE=y        # 每文件独立 FAT 缓存
CONFIG_FATFS_USE_FASTSEEK=y          # 快速 seek 表
CONFIG_FATFS_ALLOC_PREFER_ALIGNED_WORK_BUFFERS=y  # DMA 对齐
CONFIG_FATFS_ALLOC_PREFER_EXTRAM=n   # FatFS 内部缓冲放 SRAM(更快)
```

## 调试特性

### 合成彩条测试

`DEBUG_SYNTHETIC_TEST=1` 时前 10 帧用已知彩色竖条（白→黄→青→绿→红）替代摄像头数据，验证 H.264 编码器 O_UYY_E_VYY 输入格式。录像中颜色分明则格式正确，颜色错乱则 U/V 顺序反了。

### O_UYY_E_VYY 裸数据 Dump

前 5 帧 PPA 输出 (`/sdcard/dump_0.ppa` ~ `dump_4.ppa`) 和 camera_cb 输出 (`/sdcard/dump_cb_0.ppa` ~ `dump_cb_4.ppa`)。该格式即 H.264 硬件编码器所需的 O_UYY_E_VYY（UYY/VYY 行交织），可用 `tools/ouev_to_i420.py` 转换为 I420 或 PNG 查看。

播放命令:
```bash
python ouev_to_i420.py dump_0.ppa --png dump_0.png
```

### 性能日志

每 30 帧输出逐操作耗时拆解：
```
⏱ perf: total=62ms | ppa_disp=0 copy=1 h264=28 mv_proc=0 sd_queue=1
```
精确定位每一步的瓶颈变化。

## 未来蓝图

### 第一阶段: AI 接入 (进行中)

- [x] snake_detect 组件骨架 (PPA 缩放就绪, 模拟推理)
- [x] activity_tracker 活动追踪 + JSONL 日志
- [x] NTP 时间同步
- [x] 三态状态机 (MONITOR → AI_VERIFY → RECORDING)
- [ ] ESP-DL 真实推理接入 (YOLO11n INT8, 320×320)
- [ ] 模型重新校准 (50-200 张 SC2336 真实帧)

### 第二阶段: 完整功能

- [ ] zone 区域配置加载 (SD 卡 zone.conf)
- [ ] 每日 18:00 云端上传 (HTTP POST daily stats)
- [ ] 设备心跳 + 命令轮询 (后端 API 对接)
- [ ] OSD 信息叠加 (LVGL 图层叠加时间戳、状态图标)

### 第三阶段: 优化与扩展

- [ ] PPA 显示改为非阻塞模式（如硬件支持）
- [ ] 评估 `espressif/motion_detect` 替代 H.264 MV 检测
- [ ] 双向音频: ES8311 麦克风录音 + 扬声器播放
- [ ] WiFi 实时推流: RTSP/HLS 推流至手机 App
- [ ] 云端存储: 录像自动上传至阿里云 OSS / AWS S3

### 架构演进

```
当前 (单芯片全功能):
  ESP32-P4 ── 摄像头 + LCD + 编码 + 检测 + SD + (未来) WiFi/云端

未来 (可拆分):
  ESP32-P4 (边缘) ── 摄像头 + 编码 + SD 本地存储
  ESP32-S3 (连接) ── WiFi/BLE + 云端同步 + OTA 升级
```

> **设计原则**: 优先本地化处理（隐私、低延迟），云端做备份和远程访问。

---

## 构建与烧录

```bash
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

完整清理重建：
```bash
idf.py fullclean build flash monitor
```

## 参考资源

- [ESP-IDF v5.5.4 文档](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.4/esp32p4/)
- [ESP32-P4 Function EV Board BSP](https://components.espressif.com/components/espressif/esp32_p4_function_ev_board)
- [esp_h264 硬件编码器](https://components.espressif.com/components/espressif/esp_h264)
- [motion_detect 组件](https://components.espressif.com/components/espressif/motion_detect)
