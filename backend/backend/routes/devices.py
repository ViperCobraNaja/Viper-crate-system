"""设备管理路由 [重写] —— 心跳、状态、命令队列"""

import logging
from datetime import datetime, timezone
from flask import Blueprint, request, jsonify, current_app

logger = logging.getLogger(__name__)
devices_bp = Blueprint('devices', __name__)


# ==================== 心跳 ====================

@devices_bp.route('/device/heartbeat', methods=['POST'])
def device_heartbeat():
    """设备心跳上报 —— 对应固件 cloud_interface_send_heartbeat() + report_status()"""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少JSON数据"}), 400

        heartbeat = current_app.device_service.process_heartbeat(data)

        return jsonify({
            "message": "心跳已接收",
            "device_id": heartbeat.device_id,
            "server_time": datetime.now(timezone.utc).isoformat(),
        }), 200
    except Exception as e:
        logger.error(f"处理心跳时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


# ==================== 设备状态 ====================

@devices_bp.route('/device/status', methods=['POST'])
def update_device_status():
    """更新设备状态（兼容旧版接口）"""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少状态数据"}), 400

        data["update_time"] = datetime.now(timezone.utc).isoformat()
        success = current_app.storage_manager.update_device_status(data)

        return jsonify({
            "message": "设备状态更新成功" if success else "设备状态更新失败",
            "device_id": data.get("device_id"),
            "success": success
        }), 200 if success else 500
    except Exception as e:
        logger.error(f"更新设备状态时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@devices_bp.route('/device/<device_id>', methods=['GET'])
def get_device_info(device_id):
    """获取设备详情"""
    try:
        info = current_app.device_service.get_device_info(device_id)
        return jsonify(info.to_dict()), 200
    except Exception as e:
        logger.error(f"获取设备状态时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@devices_bp.route('/device/list', methods=['GET'])
def list_devices():
    """获取设备列表（从设备状态集合）"""
    try:
        db = current_app.storage_manager.db_storage
        if db:
            cursor = db.device_status.find({})
            devices = []
            for doc in cursor:
                doc['_id'] = str(doc['_id'])
                devices.append(doc)
        else:
            devices = []
        return jsonify({"count": len(devices), "devices": devices}), 200
    except Exception as e:
        logger.error(f"获取设备列表时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


# ==================== 命令队列 ====================

@devices_bp.route('/device/<device_id>/commands', methods=['GET'])
def poll_device_commands(device_id):
    """设备轮询待执行命令 —— 固件定期调用此接口获取指令"""
    try:
        commands = current_app.device_service.poll_commands(device_id)
        return jsonify({
            "device_id": device_id,
            "count": len(commands),
            "commands": commands,
        }), 200
    except Exception as e:
        logger.error(f"设备轮询命令时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@devices_bp.route('/device/<device_id>/commands', methods=['POST'])
def send_device_command(device_id):
    """向设备下发命令"""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少命令数据"}), 400

        command = data.get('command')
        params = data.get('params', {})

        if not command:
            return jsonify({"error": "缺少command字段"}), 400

        cmd_id = current_app.device_service.enqueue_command(
            device_id, command, params)

        if cmd_id:
            return jsonify({
                "message": "命令已入队",
                "command_id": cmd_id,
                "device_id": device_id,
                "command": command,
            }), 201
        else:
            return jsonify({
                "error": "命令入队失败（可能是无效命令或队列已满）"
            }), 400
    except Exception as e:
        logger.error(f"下发命令时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@devices_bp.route('/device/<device_id>/commands/<command_id>', methods=['PUT'])
def report_command_result(device_id, command_id):
    """设备上报命令执行结果"""
    try:
        data = request.get_json() or {}
        success = data.get('success', True)
        current_app.device_service.mark_command_done(command_id, success)

        return jsonify({
            "message": "命令状态已更新",
            "command_id": command_id,
            "status": "executed" if success else "failed"
        }), 200
    except Exception as e:
        logger.error(f"上报命令结果时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500
