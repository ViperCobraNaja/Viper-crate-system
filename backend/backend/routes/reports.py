"""日报/报告路由"""

import logging
from datetime import datetime, timezone, timedelta
from flask import Blueprint, request, jsonify, current_app

logger = logging.getLogger(__name__)
reports_bp = Blueprint('reports', __name__)


@reports_bp.route('/reports/daily', methods=['GET'])
def generate_daily_report():
    try:
        device_id = request.args.get('device_id')
        date_str = request.args.get('date')

        if not device_id:
            return jsonify({"error": "缺少device_id参数"}), 400

        if date_str:
            if 'T' in date_str:
                target_datetime = datetime.fromisoformat(
                    date_str.replace('Z', '+00:00'))
                target_date = target_datetime.date()
            else:
                target_date = datetime.fromisoformat(date_str).date()
        else:
            target_date = datetime.now(timezone.utc).date()

        start_time = datetime.combine(target_date, datetime.min.time()).replace(tzinfo=timezone.utc)
        end_time = datetime.combine(target_date, datetime.max.time()).replace(tzinfo=timezone.utc)

        from services.sensor_service import SensorService

        db = current_app.storage_manager.db_storage
        if db:
            sensor_data = list(db.sensor_data.find({
                "device_id": device_id,
                "timestamp": {"$gte": start_time, "$lte": end_time}
            }).sort("timestamp", 1))
            video_count = db.video_metadata.count_documents({
                "device_id": device_id,
                "create_time": {"$gte": start_time.isoformat(), "$lte": end_time.isoformat()}
            })
        else:
            sensor_data = current_app.storage_manager.get_sensor_history(
                device_id, start_time.isoformat(), end_time.isoformat(), 10000)
            video_count = 0

        report = {
            "date": target_date.isoformat(),
            "device_id": device_id,
            "summary": {
                "sensor_data_count": len(sensor_data),
                "video_count": video_count,
                "avg_temperature": SensorService.calculate_average(
                    sensor_data, "temperature"),
                "avg_humidity": SensorService.calculate_average(
                    sensor_data, "humidity"),
                "activity_detection_count": SensorService.count_activity_detections(
                    sensor_data)
            },
            "recommendations": SensorService.generate_recommendations(sensor_data),
            "generated_at": datetime.now(timezone.utc).isoformat()
        }

        if db:
            db.daily_reports.insert_one(report)

        return jsonify(report), 200
    except Exception as e:
        logger.error(f"生成日报时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@reports_bp.route('/reports/activities', methods=['GET'])
def get_activity_log():
    try:
        device_id = request.args.get('device_id')
        limit = int(request.args.get('limit', 10))

        activities = []
        db = current_app.storage_manager.db_storage
        if db and device_id:
            cursor = db.motion_events.find(
                {"device_id": device_id}
            ).sort("timestamp", -1).limit(limit)
            for doc in cursor:
                doc['_id'] = str(doc['_id'])
                activities.append(doc)

        return jsonify({"count": len(activities), "activities": activities}), 200
    except Exception as e:
        logger.error(f"获取活动记录时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500
