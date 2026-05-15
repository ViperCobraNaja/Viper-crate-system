#!/usr/bin/env python3
"""
宠语者 - 智能宠物箱后端系统
基于边缘感知与大模型日报的智能宠物箱后端应用

功能:
1. 接收 ESP32-P4 传感器数据
2. 接收运动检测事件（含 H.264 硬件运动矢量热度图）
3. 接收视频文件上传 + H.264→MP4 自动转换
4. 设备管理（心跳、状态监控、命令下发）
5. AI 处理和分析
6. Web 监控界面
"""

import os
import logging
from datetime import datetime
from pathlib import Path

from flask import Flask, jsonify, render_template
from flask_cors import CORS
import pymongo
from pymongo import MongoClient

from config import Config
from storage import DataStorageManager
from services.sensor_service import SensorService
from services.motion_service import MotionService
from services.video_service import VideoService
from services.device_service import DeviceService
from services.ai_service import AIService
from llm_service import LLMService
from routes import register_all_blueprints

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

PROJECT_ROOT = Path(__file__).parent.parent


def create_app() -> Flask:
    """Flask 应用工厂"""
    app = Flask(
        __name__,
        template_folder=str(PROJECT_ROOT / 'frontend' / 'templates'),
        static_folder=str(PROJECT_ROOT / 'frontend' / 'static'),
    )
    app.config.from_object(Config)
    CORS(app)

    # ---- MongoDB 连接 ----
    try:
        client = MongoClient(Config.MONGO_URI, serverSelectionTimeoutMS=5000)
        client.server_info()
        mongo_client = client
        logger.info("MongoDB 连接成功")
    except Exception as e:
        logger.warning(f"MongoDB 连接失败: {e}，使用无数据库模式")
        mongo_client = None

    # ---- LLM 服务 ----
    llm_service = LLMService(Config())

    # ---- 存储层 ----
    storage_manager = DataStorageManager(mongo_client)

    # ---- 业务服务层 ----
    sensor_service = SensorService()
    motion_service = MotionService(storage_manager)
    video_service = VideoService(storage_manager)
    device_service = DeviceService(storage_manager)
    ai_service = AIService(Config(), llm_service)

    # ---- 挂载到 app 对象（蓝图通过 current_app 访问）----
    app.storage_manager = storage_manager
    app.sensor_service = sensor_service
    app.motion_service = motion_service
    app.video_service = video_service
    app.device_service = device_service
    app.ai_service = ai_service
    app.llm_service = llm_service
    app.mongo_client = mongo_client

    # ---- 注册路由蓝图 ----
    register_all_blueprints(app)

    # ---- 确保数据目录存在 ----
    os.makedirs("data", exist_ok=True)
    os.makedirs("data/sensor", exist_ok=True)
    os.makedirs("data/videos", exist_ok=True)
    os.makedirs("data/configs", exist_ok=True)

    return app


app = create_app()


# ==================== 基础路由（保留在 app.py）====================

@app.route('/')
def index():
    return render_template('index.html')


@app.route('/health', methods=['GET'])
def health_check():
    return jsonify({
        "status": "healthy",
        "service": "petbox-backend",
        "version": "1.1.0",
        "timestamp": datetime.utcnow().isoformat(),
        "dependencies": {
            "mongodb": "healthy" if app.mongo_client else "unavailable",
            "flask": "healthy",
            "ai_service": "initialized" if app.ai_service else "not_initialized",
            "llm_service": app.llm_service.health_check() if app.llm_service else "unavailable",
        }
    }), 200


@app.route('/api/v1/llm/status', methods=['GET'])
def llm_status():
    return jsonify(app.llm_service.health_check()), 200


# ==================== 错误处理 ====================

@app.errorhandler(404)
def not_found(error):
    return jsonify({"error": "请求的资源不存在"}), 404


@app.errorhandler(500)
def internal_error(error):
    return jsonify({"error": "服务器内部错误"}), 500


@app.errorhandler(400)
def bad_request(error):
    return jsonify({"error": "请求参数错误"}), 400


# ==================== 启动 ====================

if __name__ == '__main__':
    print("=" * 50)
    print("  宠语者 - 智能宠物箱后端系统")
    print("  版本: 1.1.0")
    print("  启动时间:", datetime.now().isoformat())
    print("  服务地址: http://localhost:5001")
    print("  API 文档: http://localhost:5001/")
    print("=" * 50)

    app.run(
        host=Config.HOST,
        port=Config.PORT,
        debug=Config.DEBUG
    )
