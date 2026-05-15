"""
宠语者 - 传感器数据处理模块 （兼容性导出层）

数据模型已迁移至 models/sensor.py，处理逻辑已迁移至 services/sensor_service.py。
本文件保留为兼容性导出层，支持旧导入路径。
"""

import json
import random
import logging
from datetime import datetime, timezone
from typing import Dict, Any, List

from models.sensor import TemperatureData, HumidityData, LightData, MotionData, SensorData
from services.sensor_service import SensorService

logger = logging.getLogger(__name__)


class SensorDataProcessor:
    """传感器数据处理器 —— 兼容性包装"""

    def __init__(self):
        self._service = SensorService()
        logger.info("传感器数据处理器初始化完成（兼容模式）")

    def process_sensor_data(self, raw_data: Dict[str, Any]) -> Dict[str, Any]:
        return self._service.process(raw_data)

    def process_batch_data(self, batch_data: List[Dict[str, Any]]) -> Dict[str, Any]:
        results = {
            "total": len(batch_data),
            "successful": 0,
            "failed": 0,
            "errors": [],
            "processed_data": []
        }
        for i, data in enumerate(batch_data):
            try:
                processed = self.process_sensor_data(data)
                results["processed_data"].append(processed)
                results["successful"] += 1
            except ValueError as e:
                results["failed"] += 1
                results["errors"].append({"index": i, "error": str(e)})
                logger.warning(f"批处理中第{i}条数据失败: {e}")
        logger.info(f"批量数据处理完成: 成功{results['successful']}/{results['total']}")
        return results

    def create_mock_sensor_data(self, device_id: str = "ESP32-P4-001") -> Dict[str, Any]:
        mock_data = {
            "device_id": device_id,
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "sensors": {
                "temperature": {
                    "value": round(random.uniform(20, 30), 1),
                    "unit": "celsius"
                },
                "humidity": {
                    "value": round(random.uniform(40, 70), 1),
                    "unit": "percent"
                },
                "light": {
                    "value": round(random.uniform(100, 1000)),
                    "unit": "lux"
                },
                "motion": {
                    "detected": random.choice([True, False]),
                    "confidence": round(random.uniform(0.5, 0.95), 2),
                    "location": {
                        "x": random.randint(0, 640),
                        "y": random.randint(0, 480)
                    }
                }
            },
            "status": {
                "battery": random.randint(60, 100),
                "storage_available": random.randint(1024, 8192),
                "wifi_connected": True
            }
        }
        if random.choice([True, False]):
            mock_data["video_metadata"] = {
                "filename": f"video_{datetime.now().strftime('%Y%m%d_%H%M%S')}.h264",
                "duration": random.randint(5, 30),
                "resolution": "640x480",
                "ai_processed": random.choice([True, False])
            }
        return mock_data


# 兼容性：重新导出所有数据类
__all__ = [
    'SensorDataProcessor',
    'SensorData',
    'TemperatureData',
    'HumidityData',
    'LightData',
    'MotionData',
]

if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO)
    processor = SensorDataProcessor()
    mock_data = processor.create_mock_sensor_data()
    print("模拟数据:")
    print(json.dumps(mock_data, indent=2, ensure_ascii=False))
    try:
        processed = processor.process_sensor_data(mock_data)
        print("\n处理后的数据:")
        print(json.dumps(processed, indent=2, ensure_ascii=False))
        print(f"\n验证结果: 成功")
        print(f"数据ID: {processed.get('data_id')}")
        print(f"告警数量: {len(processed.get('alerts', []))}")
    except ValueError as e:
        print(f"处理失败: {e}")
