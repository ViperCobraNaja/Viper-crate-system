"""区域配置路由 —— 从 api_routes.py 迁移"""

import os
import json
import logging
from datetime import datetime, timezone
from flask import Blueprint, request, jsonify, current_app

logger = logging.getLogger(__name__)
regions_bp = Blueprint('regions', __name__)


def _config_dir():
    return os.path.join(current_app.config.get('DATA_DIR', 'data'), 'configs')


@regions_bp.route('/config/regions', methods=['GET'])
def get_region_config():
    try:
        cfg_dir = _config_dir()
        latest_path = os.path.join(cfg_dir, 'latest_region_config.json')

        if os.path.exists(latest_path):
            with open(latest_path, 'r', encoding='utf-8') as f:
                config = json.load(f)
            return jsonify(config), 200
        else:
            return jsonify({"error": "未找到区域配置"}), 404
    except Exception as e:
        logger.error(f"获取区域配置时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@regions_bp.route('/config/regions', methods=['POST'])
def save_region_config():
    try:
        config = request.get_json()
        if not config:
            return jsonify({"error": "缺少区域配置数据"}), 400

        required = ['version', 'image_width', 'image_height']
        for field in required:
            if field not in config:
                return jsonify({"error": f"缺少必需字段: {field}"}), 400

        config.setdefault('regions', [])
        if not isinstance(config['regions'], list):
            return jsonify({"error": "regions必须是数组"}), 400

        cfg_dir = _config_dir()
        os.makedirs(cfg_dir, exist_ok=True)

        timestamp = datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')
        config_filename = f'region_config_{timestamp}.json'
        config_filepath = os.path.join(cfg_dir, config_filename)

        with open(config_filepath, 'w', encoding='utf-8') as f:
            json.dump(config, f, indent=2, ensure_ascii=False)

        latest_path = os.path.join(cfg_dir, 'latest_region_config.json')
        with open(latest_path, 'w', encoding='utf-8') as f:
            json.dump(config, f, indent=2, ensure_ascii=False)

        return jsonify({
            "message": "区域配置保存成功",
            "config_file": config_filename,
            "timestamp": datetime.now(timezone.utc).isoformat()
        }), 200
    except Exception as e:
        logger.error(f"保存区域配置时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@regions_bp.route('/config/working-area', methods=['POST'])
def set_working_area():
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少工作区域数据"}), 400

        required = ['id', 'name', 'color', 'vertices']
        for field in required:
            if field not in data:
                return jsonify({"error": f"缺少必需字段: {field}"}), 400

        if not isinstance(data['vertices'], list) or len(data['vertices']) < 3:
            return jsonify({"error": "工作区域顶点数量必须大于等于3"}), 400

        cfg_dir = _config_dir()
        os.makedirs(cfg_dir, exist_ok=True)
        latest_path = os.path.join(cfg_dir, 'latest_region_config.json')

        if os.path.exists(latest_path):
            with open(latest_path, 'r', encoding='utf-8') as f:
                config = json.load(f)
        else:
            config = {
                'version': '1.0', 'image_width': 800, 'image_height': 600,
                'regions': []
            }

        config['working_area'] = {
            'id': data['id'], 'name': data['name'],
            'color': data['color'], 'vertices': data['vertices']
        }

        with open(latest_path, 'w', encoding='utf-8') as f:
            json.dump(config, f, indent=2, ensure_ascii=False)

        return jsonify({
            "message": "工作区域设置成功",
            "timestamp": datetime.now(timezone.utc).isoformat()
        }), 200
    except Exception as e:
        logger.error(f"设置工作区域时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@regions_bp.route('/region/check', methods=['POST'])
def check_point_in_region():
    try:
        data = request.get_json()
        if not data:
            return jsonify({"error": "缺少检查数据"}), 400

        x = data.get('x')
        y = data.get('y')
        if x is None or y is None:
            return jsonify({"error": "缺少坐标信息 (x, y)"}), 400

        cfg_dir = _config_dir()
        latest_path = os.path.join(cfg_dir, 'latest_region_config.json')
        if not os.path.exists(latest_path):
            return jsonify({"error": "未找到区域配置"}), 404

        with open(latest_path, 'r', encoding='utf-8') as f:
            config = json.load(f)

        def point_in_polygon(px, py, vertices):
            n = len(vertices)
            inside = False
            p1x, p1y = vertices[0]
            for i in range(1, n + 1):
                p2x, p2y = vertices[i % n]
                if py > min(p1y, p2y):
                    if py <= max(p1y, p2y):
                        if px <= max(p1x, p2x):
                            xinters = (py - p1y) * (p2x - p1x) / (p2y - p1y) + p1x
                            if p1x == p2x or px <= xinters:
                                inside = not inside
                p1x, p1y = p2x, p2y
            return inside

        working_area = config.get('working_area')
        if working_area:
            if not point_in_polygon(x, y, working_area.get('vertices', [])):
                return jsonify({
                    "x": x, "y": y,
                    "region": "OUT_OF_WORKING_AREA",
                    "region_name": "超出工作区域"
                }), 200

        result_region = None
        for region in config.get('regions', []):
            if point_in_polygon(x, y, region.get('vertices', [])):
                result_region = region
                break

        return jsonify({
            "x": x, "y": y,
            "region": result_region if result_region else "UNKNOWN",
            "region_name": result_region.get('name', 'UNKNOWN') if result_region else 'UNKNOWN'
        }), 200
    except Exception as e:
        logger.error(f"检查点在区域时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@regions_bp.route('/config', methods=['GET'])
def system_config():
    try:
        from config import Config
        cfg = Config()
        return jsonify({
            "supported_sensors": cfg.SUPPORTED_SENSORS,
            "temperature_alert_threshold": cfg.TEMPERATURE_ALERT_THRESHOLD,
            "humidity_alert_threshold": cfg.HUMIDITY_ALERT_THRESHOLD,
            "pet_detection_confidence_threshold": cfg.PET_DETECTION_CONFIDENCE_THRESHOLD,
            "device_heartbeat_interval": cfg.DEVICE_HEARTBEAT_INTERVAL
        }), 200
    except Exception as e:
        logger.error(f"获取系统配置时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500
