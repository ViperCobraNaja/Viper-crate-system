"""AI 处理路由"""

import logging
from flask import Blueprint, request, jsonify, current_app

logger = logging.getLogger(__name__)
ai_bp = Blueprint('ai', __name__)


@ai_bp.route('/ai/process', methods=['POST'])
def process_ai_request():
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少处理数据"}), 400

        if 'frame_data' in data:
            result = current_app.ai_service.process_video_frame(
                data['frame_data'], data.get('sensor_data'))
        elif 'sensor_data' in data:
            result = current_app.ai_service.process_sensor_data(
                data['sensor_data'])
        else:
            return jsonify(
                {"error": "缺少处理数据（frame_data 或 sensor_data）"}), 400

        return jsonify(result), 200
    except Exception as e:
        logger.error(f"AI处理请求时出错: {e}")
        return jsonify({"error": "AI处理失败", "details": str(e)}), 500
