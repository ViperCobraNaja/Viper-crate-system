"""
宠语者 - API路由模块
实现RESTful API端点和WebSocket通信
"""

from datetime import datetime, timedelta, timezone
from typing import Dict, Any, List, Optional
import json
import logging

from flask import Blueprint, request, jsonify, render_template, current_app
from flask_socketio import SocketIO, emit

from config import Config
from sensors import SensorDataProcessor
from storage import DataStorageManager
from ai_processing import AIProcessingPipeline, AIProcessorFactory

# 配置日志
logger = logging.getLogger(__name__)

# 创建蓝图
api_bp = Blueprint('api', __name__)

# 全局变量（在实际应用中应该使用更合适的存储方式）
socketio = None
ai_processor = None
sensor_processor = None
storage_manager = None


def init_api_routes(app, socketio_instance=None):
    """
    初始化API路由

    Args:
        app: Flask应用实例
        socketio_instance: SocketIO实例（可选）
    """
    global socketio, ai_processor, sensor_processor, storage_manager

    # 设置SocketIO
    socketio = socketio_instance

    # 初始化处理器
    ai_processor = AIProcessorFactory.create_processor(Config())
    sensor_processor = SensorDataProcessor()
    storage_manager = DataStorageManager(current_app.mongo_client if hasattr(current_app, 'mongo_client') else None)

    logger.info("API路由初始化完成")

    # 注册蓝图
    app.register_blueprint(api_bp, url_prefix='/api/v1')


@api_bp.route('/health', methods=['GET'])
def health_check():
    """健康检查接口"""
    return jsonify({
        "status": "healthy",
        "service": "petbox-backend",
        "version": "1.0.0",
        "timestamp": datetime.utcnow().isoformat(),
        "dependencies": {
            "mongodb": "healthy" if storage_manager and storage_manager.db_storage else "unavailable",
            "ai_processor": "initialized" if ai_processor else "not_initialized"
        }
    }), 200


@api_bp.route('/sensor/data', methods=['POST'])
def receive_sensor_data():
    """
    接收传感器数据
    请求体格式参考sensors.py中的SensorData类
    """
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少JSON数据"}), 400

        # 验证和处理传感器数据
        processed_data = sensor_processor.process_sensor_data(data)

        # 存储到本地和数据库
        storage_result = storage_manager.save_sensor_data(processed_data)

        # 发送WebSocket通知（如果启用）
        if socketio:
            socketio.emit('sensor_update', {
                'type': 'sensor_update',
                'data': processed_data
            })

        return jsonify({
            "message": "传感器数据接收成功",
            "data_id": processed_data.get("data_id"),
            "timestamp": processed_data.get("timestamp"),
            "storage_result": storage_result
        }), 201

    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except Exception as e:
        logger.error(f"处理传感器数据时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/sensor/history', methods=['GET'])
def get_sensor_history():
    """获取传感器历史数据"""
    try:
        # 获取查询参数
        device_id = request.args.get('device_id')
        sensor_type = request.args.get('sensor_type')
        start_time = request.args.get('start_time')
        end_time = request.args.get('end_time')
        limit = int(request.args.get('limit', 100))

        # 构建查询条件
        query = {}
        if device_id:
            query["device_id"] = device_id
        if sensor_type:
            query["sensor_type"] = sensor_type

        # 时间范围查询
        if start_time or end_time:
            query["timestamp"] = {}
            if start_time:
                query["timestamp"]["$gte"] = datetime.fromisoformat(start_time)
            if end_time:
                query["timestamp"]["$lte"] = datetime.fromisoformat(end_time)

        # 执行查询
        if storage_manager.db_storage:
            # 从数据库查询
            from pymongo import MongoClient
            client = current_app.mongo_client if hasattr(current_app, 'mongo_client') else None
            if client:
                db = client[Config.DATABASE_NAME]
                collection = db["sensor_data"]

                cursor = collection.find(query).sort("timestamp", -1).limit(limit)
                data = list(cursor)

                # 转换ObjectId为字符串
                for item in data:
                    item["_id"] = str(item["_id"])
            else:
                # 回退到本地文件查询
                data = storage_manager.get_sensor_history(device_id, start_time, end_time, limit)
        else:
            # 从本地文件查询
            data = storage_manager.get_sensor_history(device_id, start_time, end_time, limit)

        return jsonify({
            "count": len(data),
            "data": data
        }), 200

    except Exception as e:
        logger.error(f"获取历史数据时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/device/status', methods=['POST'])
def update_device_status():
    """更新设备状态"""
    try:
        status_data = request.get_json()
        if not status_data:
            return jsonify({"error": "缺少状态数据"}), 400

        # 添加时间戳
        status_data["update_time"] = datetime.utcnow().isoformat()

        # 更新存储
        success = storage_manager.update_device_status(status_data)

        # 发送WebSocket通知（如果启用）
        if socketio:
            socketio.emit('device_status', {
                'type': 'device_status',
                'data': status_data
            })

        return jsonify({
            "message": "设备状态更新成功" if success else "设备状态更新失败",
            "device_id": status_data.get("device_id"),
            "success": success
        }), 200 if success else 500

    except Exception as e:
        logger.error(f"更新设备状态时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/device/<device_id>', methods=['GET'])
def get_device_status(device_id):
    """获取设备状态"""
    try:
        if storage_manager.db_storage:
            status = storage_manager.db_storage.get_device_status(device_id)
            if status:
                return jsonify(status), 200

        return jsonify({"error": "设备未找到"}), 404

    except Exception as e:
        logger.error(f"获取设备状态时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/device/list', methods=['GET'])
def list_devices():
    """获取设备列表"""
    try:
        # 这里应该从数据库中获取所有设备的状态
        # 暂时返回模拟数据
        devices = [
            {
                "device_id": "ESP32-P4-001",
                "status": {
                    "online": True,
                    "battery": 85,
                    "storage_available": 4096,
                    "wifi_connected": True
                },
                "last_seen": datetime.utcnow().isoformat()
            }
        ]

        return jsonify({
            "count": len(devices),
            "devices": devices
        }), 200

    except Exception as e:
        logger.error(f"获取设备列表时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/video/metadata', methods=['POST'])
def upload_video_metadata():
    """上传视频元数据"""
    try:
        metadata = request.get_json()
        if not metadata:
            return jsonify({"error": "缺少元数据"}), 400

        # 添加时间戳
        metadata["upload_time"] = datetime.utcnow().isoformat()

        # 存储到本地和数据库
        result = storage_manager.save_video_metadata(metadata)

        # 发送WebSocket通知（如果启用）
        if socketio:
            socketio.emit('video_metadata', {
                'type': 'video_metadata',
                'data': metadata
            })

        return jsonify({
            "message": "视频元数据上传成功",
            "video_id": metadata.get("video_id"),
            "filename": metadata.get("filename"),
            "storage_result": result
        }), 201

    except Exception as e:
        logger.error(f"上传视频元数据时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/video/list', methods=['GET'])
def list_videos():
    """获取视频列表"""
    try:
        # 获取查询参数
        device_id = request.args.get('device_id')
        start_time = request.args.get('start_time')
        end_time = request.args.get('end_time')

        # 构建查询条件
        query = {}
        if device_id:
            query["device_id"] = device_id

        if start_time or end_time:
            query["timestamp"] = {}
            if start_time:
                query["timestamp"]["$gte"] = datetime.fromisoformat(start_time)
            if end_time:
                query["timestamp"]["$lte"] = datetime.fromisoformat(end_time)

        # 从数据库查询（如果可用）
        if storage_manager.db_storage:
            from pymongo import MongoClient
            client = current_app.mongo_client if hasattr(current_app, 'mongo_client') else None
            if client:
                db = client[Config.DATABASE_NAME]
                collection = db["video_metadata"]

                videos = list(collection.find(query).sort("timestamp", -1).limit(50))
                for video in videos:
                    video["_id"] = str(video["_id"])
            else:
                videos = []
        else:
            videos = []

        return jsonify({
            "count": len(videos),
            "videos": videos
        }), 200

    except Exception as e:
        logger.error(f"获取视频列表时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/reports/daily', methods=['GET'])
def generate_daily_report():
    """生成日报"""
    try:
        # 获取查询参数
        device_id = request.args.get('device_id', None)
        date = request.args.get('date', None)

        if not device_id:
            return jsonify({"error": "缺少device_id参数"}), 400

        if date:
            # 如果提供了具体日期，则解析为datetime对象
            if 'T' in date:  # 包含时间信息
                target_datetime = datetime.fromisoformat(date.replace('Z', '+00:00'))
                target_date = target_datetime.date()
            else:  # 只有日期信息
                target_date = datetime.fromisoformat(date).date()
        else:
            target_date = datetime.utcnow().date()

        # 计算时间范围（当天的开始和结束）
        start_time = datetime.combine(target_date, datetime.min.time()).replace(tzinfo=timezone.utc)
        end_time = datetime.combine(target_date, datetime.max.time()).replace(tzinfo=timezone.utc)

        # 从数据库获取当天的传感器数据
        if storage_manager.db_storage:
            from pymongo import MongoClient
            client = current_app.mongo_client if hasattr(current_app, 'mongo_client') else None
            if client:
                db = client[Config.DATABASE_NAME]
                sensor_collection = db["sensor_data"]

                sensor_query = {
                    "device_id": device_id,
                    "timestamp": {
                        "$gte": start_time,
                        "$lte": end_time
                    }
                }
                sensor_data = list(sensor_collection.find(sensor_query))

                # 获取当天的视频数据
                video_collection = db["video_metadata"]
                video_query = {
                    "device_id": device_id,
                    "timestamp": {
                        "$gte": start_time,
                        "$lte": end_time
                    }
                }
                video_data = list(video_collection.find(video_query))
            else:
                # 回退到本地存储
                sensor_data = storage_manager.get_sensor_history(
                    device_id,
                    start_time.isoformat(),
                    end_time.isoformat()
                )
                video_data = []
        else:
            # 从本地存储获取数据
            sensor_data = storage_manager.get_sensor_history(
                device_id,
                start_time.isoformat(),
                end_time.isoformat()
            )
            video_data = []

        # 生成日报（简化版本，后续可以集成大模型）
        report = {
            "date": target_date.isoformat(),
            "device_id": device_id,
            "summary": {
                "sensor_data_count": len(sensor_data),
                "video_count": len(video_data),
                "avg_temperature": calculate_average(sensor_data, "temperature"),
                "avg_humidity": calculate_average(sensor_data, "humidity"),
                "activity_detection_count": count_activity_detections(sensor_data)
            },
            "recommendations": generate_recommendations(sensor_data),
            "generated_at": datetime.utcnow().isoformat()
        }

        # 保存日报（如果数据库可用）
        if storage_manager.db_storage:
            from pymongo import MongoClient
            client = current_app.mongo_client if hasattr(current_app, 'mongo_client') else None
            if client:
                db = client[Config.DATABASE_NAME]
                reports_collection = db["daily_reports"]
                reports_collection.insert_one(report)

        return jsonify(report), 200

    except Exception as e:
        logger.error(f"生成日报时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/reports/activities', methods=['GET'])
def get_activity_log():
    """获取活动记录"""
    try:
        # 获取查询参数
        device_id = request.args.get('device_id')
        limit = int(request.args.get('limit', 10))

        # 这里应该从数据库获取AI处理的历史活动记录
        # 暂时返回模拟数据
        activities = []

        # 从数据库查询（如果可用）
        if storage_manager.db_storage:
            from pymongo import MongoClient
            client = current_app.mongo_client if hasattr(current_app, 'mongo_client') else None
            if client:
                db = client[Config.DATABASE_NAME]
                # 这里假设有一个activities集合存储AI处理结果
                # 暂时使用传感器数据来模拟活动记录
                pass

        # 如果没有数据库或查询失败，返回一些模拟数据
        if not activities:
            for i in range(min(limit, 5)):
                activities.append({
                    "activity_type": "resting",
                    "confidence": 0.85,
                    "duration": 300.0,
                    "timestamp": (datetime.utcnow() - timedelta(minutes=i*10)).isoformat(),
                    "pet_type": "cat"
                })

        return jsonify({
            "count": len(activities),
            "activities": activities
        }), 200

    except Exception as e:
        logger.error(f"获取活动记录时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/ai/process', methods=['POST'])
def process_ai_request():
    """AI处理请求"""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少处理数据"}), 400

        # 检查是否有视频帧数据
        if 'frame_data' in data:
            # 处理视频帧
            frame_data = data['frame_data']
            sensor_data = data.get('sensor_data')

            result = ai_processor.process_video_frame(frame_data, sensor_data)
        elif 'sensor_data' in data:
            # 仅处理传感器数据
            sensor_data = data['sensor_data']
            result = ai_processor.process_sensor_data(sensor_data)
        else:
            return jsonify({"error": "缺少处理数据（frame_data 或 sensor_data）"}), 400

        # 发送WebSocket通知（如果启用）
        if socketio and result.get('success', False):
            socketio.emit('activity', {
                'type': 'activity',
                'data': result
            })

        return jsonify(result), 200

    except Exception as e:
        logger.error(f"AI处理请求时出错: {e}")
        return jsonify({"error": "AI处理失败", "details": str(e)}), 500


@api_bp.route('/config', methods=['GET', 'POST'])
def system_config():
    """系统配置接口"""
    try:
        if request.method == 'GET':
            # 返回当前配置
            config = Config()
            config_dict = {
                "supported_sensors": config.SUPPORTED_SENSORS,
                "temperature_alert_threshold": config.TEMPERATURE_ALERT_THRESHOLD,
                "humidity_alert_threshold": config.HUMIDITY_ALERT_THRESHOLD,
                "pet_detection_confidence_threshold": config.PET_DETECTION_CONFIDENCE_THRESHOLD,
                "device_heartbeat_interval": config.DEVICE_HEARTBEAT_INTERVAL
            }

            return jsonify(config_dict), 200

        elif request.method == 'POST':
            # 更新配置（在实际应用中，这可能需要更严格的安全检查）
            new_config = request.get_json()
            if not new_config:
                return jsonify({"error": "缺少配置数据"}), 400

            # 在这里可以实现配置更新逻辑
            # 注意：实际应用中需要验证配置的有效性并进行持久化存储
            updated_config = {
                "message": "配置更新成功",
                "updated_fields": list(new_config.keys()),
                "timestamp": datetime.utcnow().isoformat()
            }

            return jsonify(updated_config), 200

    except Exception as e:
        logger.error(f"处理配置请求时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@api_bp.route('/config/regions', methods=['POST'])
def save_region_config():
    """保存区域配置"""
    try:
        config = request.get_json()
        if not config:
            return jsonify({"error": "缺少区域配置数据"}), 400

        # 验证配置格式
        required_fields = ['version', 'image_width', 'image_height']
        for field in required_fields:
            if field not in config:
                return jsonify({"error": f"缺少必需字段: {field}"}), 400

        # 验证regions数组（现在可能是空的）
        if 'regions' not in config:
            config['regions'] = []

        if not isinstance(config['regions'], list):
            return jsonify({"error": "regions必须是数组"}), 400

        # 检查是否有工作区域
        working_area = config.get('working_area')

        # 验证工作区域（如果存在）
        if working_area:
            required_working_area_fields = ['id', 'name', 'color', 'vertices']
            for field in required_working_area_fields:
                if field not in working_area:
                    return jsonify({"error": f"工作区域缺少必需字段: {field}"}), 400

            # 验证顶点数量
            if not isinstance(working_area['vertices'], list) or len(working_area['vertices']) < 3:
                return jsonify({"error": "工作区域顶点数量必须大于等于3"}), 400

        # 验证每个普通区域
        for i, region in enumerate(config['regions']):
            required_region_fields = ['id', 'name', 'color', 'vertices']
            for field in required_region_fields:
                if field not in region:
                    return jsonify({"error": f"区域[{i}]缺少必需字段: {field}"}), 400

            # 验证顶点数量
            if not isinstance(region['vertices'], list) or len(region['vertices']) < 3:
                return jsonify({"error": f"区域[{i}]顶点数量必须大于等于3"}), 400

        # 保存配置到本地存储（实际应用中可能需要保存到数据库）
        import os
        import json

        # 确保配置目录存在
        config_dir = os.path.join(Config.DATA_DIR, 'configs')
        os.makedirs(config_dir, exist_ok=True)

        # 生成配置文件名
        timestamp = datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')
        config_filename = f'region_config_{timestamp}.json'
        config_filepath = os.path.join(config_dir, config_filename)

        # 保存配置
        with open(config_filepath, 'w', encoding='utf-8') as f:
            json.dump(config, f, indent=2, ensure_ascii=False)

        # 同时保存为最新配置
        latest_config_path = os.path.join(config_dir, 'latest_region_config.json')
        with open(latest_config_path, 'w', encoding='utf-8') as f:
            json.dump(config, f, indent=2, ensure_ascii=False)

        return jsonify({
            "message": "区域配置保存成功",
            "config_file": config_filename,
            "timestamp": datetime.utcnow().isoformat()
        }), 200

    except Exception as e:
        logger.error(f"保存区域配置时出错: {e}")
        return jsonify({"error": "内部服务器错误", "details": str(e)}), 500


@api_bp.route('/config/regions', methods=['GET'])
def get_region_config():
    """获取区域配置"""
    try:
        import os
        import json

        # 尝试获取最新配置
        config_dir = os.path.join(Config.DATA_DIR, 'configs')
        latest_config_path = os.path.join(config_dir, 'latest_region_config.json')

        if os.path.exists(latest_config_path):
            with open(latest_config_path, 'r', encoding='utf-8') as f:
                config = json.load(f)

            return jsonify(config), 200
        else:
            return jsonify({"error": "未找到区域配置"}), 404

    except Exception as e:
        logger.error(f"获取区域配置时出错: {e}")
        return jsonify({"error": "内部服务器错误", "details": str(e)}), 500


@api_bp.route('/config/working-area', methods=['POST'])
def set_working_area():
    """设置工作区域"""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少工作区域数据"}), 400

        required_fields = ['id', 'name', 'color', 'vertices']
        for field in required_fields:
            if field not in data:
                return jsonify({"error": f"缺少必需字段: {field}"}), 400

        # 验证顶点数量
        if not isinstance(data['vertices'], list) or len(data['vertices']) < 3:
            return jsonify({"error": "工作区域顶点数量必须大于等于3"}), 400

        # 获取当前配置
        import os
        import json

        config_dir = os.path.join(Config.DATA_DIR, 'configs')
        latest_config_path = os.path.join(config_dir, 'latest_region_config.json')

        if os.path.exists(latest_config_path):
            with open(latest_config_path, 'r', encoding='utf-8') as f:
                config = json.load(f)
        else:
            # 如果没有现有配置，创建新配置
            config = {
                'version': '1.0',
                'image_width': 800,
                'image_height': 600,
                'regions': []
            }

        # 更新工作区域
        config['working_area'] = {
            'id': data['id'],
            'name': data['name'],
            'color': data['color'],
            'vertices': data['vertices']
        }

        # 保存更新后的配置
        with open(latest_config_path, 'w', encoding='utf-8') as f:
            json.dump(config, f, indent=2, ensure_ascii=False)

        return jsonify({
            "message": "工作区域设置成功",
            "timestamp": datetime.utcnow().isoformat()
        }), 200

    except Exception as e:
        logger.error(f"设置工作区域时出错: {e}")
        return jsonify({"error": "内部服务器错误", "details": str(e)}), 500


@api_bp.route('/region/check', methods=['POST'])
def check_point_in_region():
    """检查点是否在某个区域内"""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少检查数据"}), 400

        x = data.get('x')
        y = data.get('y')

        if x is None or y is None:
            return jsonify({"error": "缺少坐标信息 (x, y)"}), 400

        # 获取当前区域配置
        import os
        import json

        config_dir = os.path.join(Config.DATA_DIR, 'configs')
        latest_config_path = os.path.join(config_dir, 'latest_region_config.json')

        if not os.path.exists(latest_config_path):
            return jsonify({"error": "未找到区域配置"}), 404

        with open(latest_config_path, 'r', encoding='utf-8') as f:
            config = json.load(f)

        # 实现点在多边形算法（Python版本）
        def point_in_polygon(px, py, vertices):
            """Python实现的点在多边形算法（射线投射法）"""
            n = len(vertices)
            inside = False

            # 将第一个顶点作为前一个顶点
            p1x, p1y = vertices[0]
            for i in range(1, n + 1):
                p2x, p2y = vertices[i % n]

                # 检查点是否在当前边的y范围内
                if py > min(p1y, p2y):
                    if py <= max(p1y, p2y):
                        # 检查点是否在当前边的左侧
                        if px <= max(p1x, p2x):
                            # 计算交点x坐标
                            xinters = (py - p1y) * (p2x - p1x) / (p2y - p1y) + p1x
                            if p1x == p2x or px <= xinters:
                                inside = not inside
                p1x, p1y = p2x, p2y

            return inside

        # 检查是否在工作区域内
        working_area = config.get('working_area')
        if working_area:
            if not point_in_polygon(x, y, working_area.get('vertices', [])):
                return jsonify({
                    "x": x,
                    "y": y,
                    "region": "OUT_OF_WORKING_AREA",
                    "region_name": "超出工作区域"
                }), 200

        # 检查点在哪个区域内
        result_region = None
        for region in config.get('regions', []):
            if point_in_polygon(x, y, region.get('vertices', [])):
                result_region = region
                break

        return jsonify({
            "x": x,
            "y": y,
            "region": result_region if result_region else "UNKNOWN",
            "region_name": result_region.get('name', 'UNKNOWN') if result_region else 'UNKNOWN'
        }), 200

    except Exception as e:
        logger.error(f"检查点在区域时出错: {e}")
        return jsonify({"error": "内部服务器错误", "details": str(e)}), 500


# WebSocket事件处理（如果启用了socketio）
def register_websocket_handlers(socketio_instance):
    """注册WebSocket事件处理器"""
    @socketio_instance.on('connect')
    def handle_connect():
        logger.info(f"WebSocket客户端连接: {request.sid}")
        emit('connection', {'message': 'Connected to PetBox server'})

    @socketio_instance.on('disconnect')
    def handle_disconnect():
        logger.info(f"WebSocket客户端断开连接: {request.sid}")

    @socketio_instance.on('ping')
    def handle_ping():
        emit('pong', {'timestamp': datetime.utcnow().isoformat()})


def calculate_average(sensor_data: List[Dict], field: str) -> Optional[float]:
    """计算传感器数据的平均值"""
    values = []
    for data in sensor_data:
        if "sensors" in data and field in data["sensors"]:
            value = data["sensors"][field].get("value")
            if value is not None:
                values.append(float(value))

    return sum(values) / len(values) if values else None


def count_activity_detections(sensor_data: List[Dict]) -> int:
    """统计活动检测次数"""
    count = 0
    for data in sensor_data:
        if "sensors" in data and "motion" in data["sensors"]:
            if data["sensors"]["motion"].get("detected", False):
                count += 1
    return count


def generate_recommendations(sensor_data: List[Dict]) -> List[str]:
    """生成宠物护理建议"""
    recommendations = []

    # 分析温度数据
    temp_values = []
    for data in sensor_data:
        if "sensors" in data and "temperature" in data["sensors"]:
            value = data["sensors"]["temperature"].get("value")
            if value:
                temp_values.append(float(value))

    if temp_values:
        avg_temp = sum(temp_values) / len(temp_values)
        if avg_temp > 28:
            recommendations.append("当前温度偏高，建议开启空调或风扇")
        elif avg_temp < 18:
            recommendations.append("当前温度偏低，建议开启暖气")

    # 分析活动数据
    activity_count = count_activity_detections(sensor_data)
    if activity_count < 10:
        recommendations.append("宠物活动较少，建议增加互动玩具")
    elif activity_count > 50:
        recommendations.append("宠物活动频繁，状态良好")

    return recommendations


# 错误处理
@api_bp.errorhandler(404)
def not_found(error):
    return jsonify({"error": "请求的API端点不存在"}), 404


@api_bp.errorhandler(500)
def internal_error(error):
    logger.error(f"API内部错误: {error}")
    return jsonify({"error": "服务器内部错误"}), 500


@api_bp.errorhandler(400)
def bad_request(error):
    return jsonify({"error": "请求参数错误"}), 400


# 模块测试代码
if __name__ == '__main__':
    # 这里可以添加测试代码
    print("API路由模块测试")

    # 测试辅助函数
    test_data = [
        {
            "timestamp": "2024-01-01T12:00:00Z",
            "sensors": {
                "temperature": {"value": 25.0},
                "humidity": {"value": 60.0},
                "motion": {"detected": True}
            }
        },
        {
            "timestamp": "2024-01-01T12:01:00Z",
            "sensors": {
                "temperature": {"value": 26.0},
                "humidity": {"value": 62.0},
                "motion": {"detected": False}
            }
        }
    ]

    avg_temp = calculate_average(test_data, "temperature")
    avg_hum = calculate_average(test_data, "humidity")
    activity_count = count_activity_detections(test_data)
    recommendations = generate_recommendations(test_data)

    print(f"平均温度: {avg_temp}")
    print(f"平均湿度: {avg_hum}")
    print(f"活动检测次数: {activity_count}")
    print(f"建议: {recommendations}")