# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

"宠语者"是一个基于边缘感知与大模型日报的智能宠物箱后端系统。系统处理来自 ESP32-P4 设备的数据：
- **传感器数据**: 温度、湿度、光照
- **运动检测事件**: 基于 H.264 硬件编码器运动矢量的热度图（8x6 网格，48 维数据）
- **视频文件**: H.264 裸流录制文件，服务端自动转换为 MP4
- **设备遥测**: CPU/内存/存储使用率、芯片温度、运行时长

提供数据存储（本地文件 + MongoDB）、AI 处理、Web 监控、远程命令下发和日报生成功能。

## 常用开发命令

### 激活 Python 环境
```bash
source ~/myenv/bin/activate
```

### 后端开发运行
```bash
cd backend
pip install -r requirements.txt
python app.py    # 默认端口 5001
```

### Docker 开发环境
```bash
docker-compose -f docker-compose.dev.yml up --build
docker-compose -f docker-compose.dev.yml up -d --build   # 后台运行
```

### 生产环境部署
```bash
docker-compose up -d --build
docker-compose ps
docker-compose logs -f backend
```

### API 测试
```bash
# 健康检查
curl http://localhost:5001/health

# 传感器数据（旧版兼容）
curl -X POST http://localhost:5001/api/v1/sensor/data \
  -H "Content-Type: application/json" \
  -d '{"device_id":"ESP32-P4-001","timestamp":"2024-01-01T12:00:00Z","sensors":{"temperature":{"value":25.5,"unit":"celsius"}}}'

# 运动检测事件 [新]
curl -X POST http://localhost:5001/api/v1/motion/events \
  -H "Content-Type: application/json" \
  -d '{"device_id":"ESP32-P4-001","timestamp":1715000422000,"motion_level":45,"heatmap":{"grid_motion":[...],"total_motion":328,...}}'

# 设备心跳 [新]
curl -X POST http://localhost:5001/api/v1/device/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"device_id":"ESP32-P4-001","cpu_usage":45,"memory_usage":62,"storage_usage":30,"temperature":48.5,"uptime_ms":3600000}'

# 设备命令下发 [新]
curl -X POST http://localhost:5001/api/v1/device/ESP32-P4-001/commands \
  -H "Content-Type: application/json" \
  -d '{"command":"start_recording","params":{"duration":60}}'

# 设备轮询命令 [新]
curl http://localhost:5001/api/v1/device/ESP32-P4-001/commands

# 视频上传 [新]
curl -X POST http://localhost:5001/api/v1/video/upload \
  -F "file=@test.h264" \
  -F "device_id=ESP32-P4-001" \
  -F 'metadata={"filename":"test.h264","width":1280,"height":960,"fps":20}'
```

## 系统架构（2026-05 重构后）

```
backend/
├── app.py                     # Flask 应用工厂（~110行），服务初始化 + 蓝图注册
├── config.py                  # 统一配置（含视频/运动/设备新配置项）
│
├── models/                    # [新] 纯数据结构定义
│   ├── sensor.py              #   SensorData, TemperatureData 等
│   ├── motion.py              #   MotionHeatmap (8×6网格), MotionEvent
│   ├── video.py               #   VideoMetadata, VideoUploadResult
│   └── device.py              #   DeviceHeartbeat, DeviceCommand, DeviceStatus
│
├── routes/                    # [新] Flask Blueprint 路由层
│   ├── __init__.py            #   统一注册所有蓝图
│   ├── sensors.py             #   /api/v1/sensor/*
│   ├── motion.py              #   /api/v1/motion/*         [新]
│   ├── videos.py              #   /api/v1/video/*          [重写]
│   ├── devices.py             #   /api/v1/device/*         [重写]
│   ├── reports.py             #   /api/v1/reports/*
│   ├── ai.py                  #   /api/v1/ai/*
│   └── regions.py             #   /api/v1/config/*, /api/v1/region/*
│
├── services/                  # [新] 业务逻辑层
│   ├── sensor_service.py      #   传感器验证 + 告警分析
│   ├── motion_service.py      #   运动事件接收 + 热度图趋势聚合 [新]
│   ├── video_service.py       #   视频上传 + H.264→MP4 转换调度 [新]
│   ├── device_service.py      #   心跳处理 + 命令队列 + 离线检测  [新]
│   └── ai_service.py          #   AI 处理流水线
│
├── storage/                   # [重构] 存储层
│   ├── __init__.py            #   DataStorageManager 统一门面
│   ├── file_storage.py        #   本地 JSON 文件 CRUD
│   ├── db_storage.py          #   MongoDB 操作（含运动事件、心跳、命令队列）
│   └── video_storage.py       #   视频二进制文件系统存储        [新]
│
├── llm/                       # [不动] LLM 模块
│   ├── service.py             #   (即 llm_service.py)
│   └── models.py              #   (即 llm_models.py)
│
└── utils/
    ├── image_utils.py         # [不动] 图片工具
    └── video_utils.py         # [新] ffmpeg 封装 (H.264→MP4)

兼容性文件:
  sensors.py                   # 兼容层，重新导出 models/sensor.py + SensorService
  api_routes.py                # 已弃用，路由已迁移至 routes/
  ai_processing.py             # 已弃用，逻辑已迁移至 services/ai_service.py
  storage.py                   # 已弃用，逻辑已迁移至 storage/
```

### 数据流
```
ESP32-P4 设备
    ├── (POST) 传感器数据 → /api/v1/sensor/data
    │       → SensorService.process() → DataStorageManager.save_sensor_data()
    │           ├── 本地 JSON → data/sensor/
    │           └── MongoDB → petbox_db.sensor_data
    │
    ├── (POST) 运动事件 → /api/v1/motion/events
    │       → MotionService.ingest_event()
    │           └── MongoDB → petbox_db.motion_events
    │
    ├── (POST) 视频文件 → /api/v1/video/upload   [multipart]
    │       → VideoService.receive_upload()
    │           ├── 原始 H.264 → data/videos/{device_id}/{date}/{id}.h264
    │           ├── ffmpeg 转 MP4 → data/videos/{device_id}/{date}/{id}.mp4
    │           └── 元数据 → MongoDB → petbox_db.video_metadata
    │
    ├── (POST) 心跳 → /api/v1/device/heartbeat
    │       → DeviceService.process_heartbeat()
    │           └── MongoDB → petbox_db.device_heartbeats + device_status
    │
    └── (GET) 命令轮询 → /api/v1/device/{id}/commands
            → DeviceService.poll_commands()
                └── MongoDB → petbox_db.device_commands
```

### 存储策略
- **本地 JSON**: `data/sensor/`、`data/videos_meta/` — 传感器数据和视频元数据的文本备份
- **本地二进制**: `data/videos/{device_id}/{date}/` — 视频 H.264/MP4 文件
- **MongoDB**: 主数据库 `petbox_db`
  - `sensor_data`, `video_metadata`, `daily_reports` — 原有
  - `device_status`, `device_heartbeats` — 设备管理
  - `motion_events` — 运动检测事件 [新]
  - `device_commands` — 命令队列 [新]
- **离线模式**: MongoDB 不可用时，JSON 数据降级到本地文件；命令队列和运动事件需要 MongoDB

## 完整 API 端点

### 基础
- `GET  /` — Web 监控界面
- `GET  /health` — 健康检查
- `GET  /api/v1/llm/status` — LLM 服务状态

### 传感器数据
- `POST /api/v1/sensor/data` — 上报传感器数据
- `GET  /api/v1/sensor/history` — 查询历史（?device_id=&start_time=&end_time=&limit=）

### 运动检测 [新]
- `POST /api/v1/motion/events` — 设备上报运动事件（含热度图 48 格数据）
- `GET  /api/v1/motion/events` — 查询运动事件历史
- `GET  /api/v1/motion/trend` — 热度图趋势分析（?device_id=&hours=24）

### 视频管理 [重写]
- `POST /api/v1/video/upload` — multipart 上传视频文件 + 元数据
- `POST /api/v1/video/metadata` — 仅上传元数据（旧版兼容）
- `GET  /api/v1/video/list` — 视频列表
- `GET  /api/v1/video/<id>/play` — MP4 视频流播放
- `DELETE /api/v1/video/<id>` — 删除视频

### 设备管理 [重写]
- `POST /api/v1/device/heartbeat` — 心跳 + 遥测数据 [新]
- `POST /api/v1/device/status` — 更新设备状态
- `GET  /api/v1/device/<id>` — 设备详情（含在线状态）
- `GET  /api/v1/device/list` — 设备列表
- `GET  /api/v1/device/<id>/commands` — 设备轮询待执行命令 [新]
- `POST /api/v1/device/<id>/commands` — 向设备下发命令 [新]
- `PUT  /api/v1/device/<id>/commands/<cmd_id>` — 设备上报命令执行结果 [新]

### 报告
- `GET  /api/v1/reports/daily` — 日报
- `GET  /api/v1/reports/activities` — 活动记录（从运动事件读取）

### AI 处理
- `POST /api/v1/ai/process` — AI 处理（帧数据或传感器数据）

### 区域配置
- `GET  /api/v1/config` — 系统配置
- `GET  /api/v1/config/regions` — 获取区域配置
- `POST /api/v1/config/regions` — 保存区域配置
- `POST /api/v1/config/working-area` — 设置工作区域
- `POST /api/v1/region/check` — 检查坐标所属区域

## 关键数据格式

### 运动事件
```json
{
  "device_id": "ESP32-P4-001",
  "timestamp": 1715000422000,
  "motion_level": 45,
  "heatmap": {
    "grid_motion": [0,0,5,23,...,0],
    "grid_count": [0,0,2,8,...,0],
    "total_motion": 328,
    "total_valid": 45,
    "avg_motion": 7.29,
    "hot_zones": [12,13,20,21],
    "frame_timestamp": 1715000422
  },
  "snapshot": "<optional base64>"
}
```

### 设备心跳
```json
{
  "device_id": "ESP32-P4-001",
  "cpu_usage": 45,
  "memory_usage": 62,
  "storage_usage": 30,
  "temperature": 48.5,
  "uptime_ms": 3600000
}
```

### 设备命令
```json
{
  "command": "start_recording",
  "params": {"duration": 60}
}
```
支持的命令: `start_recording`、`stop_recording`、`reboot`

## 开发注意事项

1. **环境变量**: 使用 `config.py` 中的 Config 类，支持环境变量覆盖
2. **MongoDB 降级**: MongoDB 连接失败时，传感器/视频元数据降级到本地文件，命令队列和运动事件不可用
3. **ffmpeg 依赖**: 视频上传功能需要系统安装 ffmpeg（macOS: `brew install ffmpeg`，Ubuntu: `apt install ffmpeg`）
4. **端口**: 默认运行在端口 5001
5. **数据验证**: 所有传感器数据在接收时验证范围和格式
6. **LLM 配置**: 通过 `.env` 文件配置 API 密钥
7. **虚拟环境**: 运行前先激活 `source ~/myenv/bin/activate`
