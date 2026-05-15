"""传感器数据路由 —— 保留旧版接口兼容"""

import logging
from flask import Blueprint, request, jsonify, current_app

from services.sensor_service import SensorService
from storage import DataStorageManager

logger = logging.getLogger(__name__)
sensors_bp = Blueprint('sensors', __name__)


def get_services():
    """从 Flask app context 获取服务实例"""
    return current_app.sensor_service, current_app.storage_manager


@sensors_bp.route('/sensor/data', methods=['POST'])
def receive_sensor_data():
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少JSON数据"}), 400

        sensor_svc, storage_mgr = get_services()
        processed = sensor_svc.process(data)
        storage_result = storage_mgr.save_sensor_data(processed)

        return jsonify({
            "message": "传感器数据接收成功",
            "data_id": processed.get("data_id"),
            "timestamp": processed.get("timestamp"),
            "storage_result": storage_result
        }), 201
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    except Exception as e:
        logger.error(f"处理传感器数据时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@sensors_bp.route('/sensor/history', methods=['GET'])
def get_sensor_history():
    try:
        device_id = request.args.get('device_id')
        start_time = request.args.get('start_time')
        end_time = request.args.get('end_time')
        limit = int(request.args.get('limit', 100))

        _, storage_mgr = get_services()
        data = storage_mgr.get_sensor_history(
            device_id, start_time, end_time, limit)

        return jsonify({"count": len(data), "data": data}), 200
    except Exception as e:
        logger.error(f"获取历史数据时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500
