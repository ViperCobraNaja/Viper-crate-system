"""运动检测事件路由 [新] —— 对应固件 cloud_interface_report_motion()"""

import logging
from flask import Blueprint, request, jsonify, current_app

logger = logging.getLogger(__name__)
motion_bp = Blueprint('motion', __name__)


@motion_bp.route('/motion/events', methods=['POST'])
def report_motion_event():
    """设备上报运动检测事件（含热度图）"""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少JSON数据"}), 400

        event = current_app.motion_service.ingest_event(data)

        return jsonify({
            "message": "运动事件已记录",
            "device_id": event.device_id,
            "motion_level": event.motion_level,
            "timestamp": event.timestamp.isoformat(),
        }), 201
    except Exception as e:
        logger.error(f"处理运动事件时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@motion_bp.route('/motion/events', methods=['GET'])
def query_motion_events():
    """查询运动事件历史"""
    try:
        device_id = request.args.get('device_id')
        start_time = request.args.get('start_time')
        end_time = request.args.get('end_time')
        limit = int(request.args.get('limit', 100))

        events = current_app.motion_service.query_events(
            device_id, start_time, end_time, limit)

        return jsonify({"count": len(events), "events": events}), 200
    except Exception as e:
        logger.error(f"查询运动事件时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@motion_bp.route('/motion/trend', methods=['GET'])
def get_motion_trend():
    """获取运动热度图趋势分析"""
    try:
        device_id = request.args.get('device_id')
        hours = int(request.args.get('hours', 24))

        if not device_id:
            return jsonify({"error": "缺少device_id参数"}), 400

        trend = current_app.motion_service.get_heatmap_trend(device_id, hours)
        return jsonify(trend), 200
    except Exception as e:
        logger.error(f"获取运动趋势时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500
