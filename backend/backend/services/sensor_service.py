"""传感器数据处理服务"""

import logging
from datetime import datetime, timezone
from typing import Dict, Any, List

from config import Config
from models.sensor import SensorData

logger = logging.getLogger(__name__)


class SensorService:
    """传感器数据业务逻辑"""

    def __init__(self):
        self.config = Config()
        logger.info("传感器服务初始化完成")

    def process(self, raw_data: Dict[str, Any]) -> Dict[str, Any]:
        sensor_data = SensorData.from_json(raw_data)
        is_valid, errors = sensor_data.validate()
        if not is_valid:
            raise ValueError(f"传感器数据验证失败: {', '.join(errors)}")

        if not sensor_data.data_id:
            sensor_data.data_id = sensor_data.generate_data_id()

        alerts = sensor_data.analyze_for_alerts()
        processed = sensor_data.to_json()
        processed['_processed'] = True
        processed['_processing_time'] = datetime.now(timezone.utc).isoformat()
        processed['alerts'] = alerts
        processed['summary'] = sensor_data.get_summary()

        logger.info(f"传感器数据处理完成: {sensor_data.device_id}")
        return processed

    @staticmethod
    def calculate_average(sensor_data_list: List[Dict], field: str) -> float:
        values = []
        for data in sensor_data_list:
            if "sensors" in data and field in data["sensors"]:
                value = data["sensors"][field].get("value")
                if value is not None:
                    values.append(float(value))
        return sum(values) / len(values) if values else None

    @staticmethod
    def count_activity_detections(sensor_data_list: List[Dict]) -> int:
        count = 0
        for data in sensor_data_list:
            if "sensors" in data and "motion" in data["sensors"]:
                if data["sensors"]["motion"].get("detected", False):
                    count += 1
        return count

    @staticmethod
    def generate_recommendations(sensor_data_list: List[Dict]) -> List[str]:
        recommendations = []
        temp_values = []
        for data in sensor_data_list:
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
        activity_count = SensorService.count_activity_detections(sensor_data_list)
        if activity_count < 10:
            recommendations.append("宠物活动较少，建议增加互动玩具")
        elif activity_count > 50:
            recommendations.append("宠物活动频繁，状态良好")
        return recommendations
