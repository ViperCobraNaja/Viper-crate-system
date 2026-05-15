"""传感器数据模型 —— 从旧版 sensors.py 迁移的 dataclass"""

import json
import hashlib
from datetime import datetime, timezone
from typing import Dict, Any, Optional, List, Tuple
from dataclasses import dataclass, asdict

from config import Config


@dataclass
class TemperatureData:
    value: float
    unit: str = "celsius"
    timestamp: Optional[datetime] = None
    sensor_id: Optional[str] = None

    def validate(self) -> Tuple[bool, str]:
        cfg = Config()
        min_t = cfg.SUPPORTED_SENSORS['temperature']['min']
        max_t = cfg.SUPPORTED_SENSORS['temperature']['max']
        if self.value < min_t or self.value > max_t:
            return False, f"温度值{self.value}超出范围[{min_t}, {max_t}]"
        return True, "温度数据有效"


@dataclass
class HumidityData:
    value: float
    unit: str = "percent"
    timestamp: Optional[datetime] = None
    sensor_id: Optional[str] = None

    def validate(self) -> Tuple[bool, str]:
        cfg = Config()
        min_h = cfg.SUPPORTED_SENSORS['humidity']['min']
        max_h = cfg.SUPPORTED_SENSORS['humidity']['max']
        if self.value < min_h or self.value > max_h:
            return False, f"湿度值{self.value}超出范围[{min_h}, {max_h}]"
        return True, "湿度数据有效"


@dataclass
class LightData:
    value: float
    unit: str = "lux"
    timestamp: Optional[datetime] = None
    sensor_id: Optional[str] = None

    def validate(self) -> Tuple[bool, str]:
        cfg = Config()
        min_l = cfg.SUPPORTED_SENSORS['light']['min']
        max_l = cfg.SUPPORTED_SENSORS['light']['max']
        if self.value < min_l or self.value > max_l:
            return False, f"光照值{self.value}超出范围[{min_l}, {max_l}]"
        return True, "光照数据有效"


@dataclass
class MotionData:
    detected: bool
    confidence: float
    location: Optional[Dict[str, int]] = None
    timestamp: Optional[datetime] = None
    sensor_id: Optional[str] = None

    def validate(self) -> Tuple[bool, str]:
        if self.confidence < 0 or self.confidence > 1:
            return False, f"置信度{self.confidence}超出范围[0, 1]"
        if self.location:
            if 'x' not in self.location or 'y' not in self.location:
                return False, "位置数据缺少x或y坐标"
        return True, "运动数据有效"


@dataclass
class SensorData:
    device_id: str
    timestamp: datetime
    sensors: Dict[str, Any]
    video_metadata: Optional[Dict[str, Any]] = None
    status: Optional[Dict[str, Any]] = None
    data_id: Optional[str] = None

    @classmethod
    def from_json(cls, json_data: Dict[str, Any]) -> 'SensorData':
        if 'timestamp' in json_data:
            if isinstance(json_data['timestamp'], str):
                timestamp = datetime.fromisoformat(
                    json_data['timestamp'].replace('Z', '+00:00'))
            else:
                timestamp = datetime.fromtimestamp(
                    json_data['timestamp'], tz=timezone.utc)
        else:
            timestamp = datetime.now(timezone.utc)

        return cls(
            device_id=json_data.get('device_id', 'unknown'),
            timestamp=timestamp,
            sensors=json_data.get('sensors', {}),
            video_metadata=json_data.get('video_metadata'),
            status=json_data.get('status'),
            data_id=json_data.get('data_id')
        )

    def to_json(self) -> Dict[str, Any]:
        result = {
            "device_id": self.device_id,
            "timestamp": self.timestamp.isoformat(),
            "sensors": self.sensors,
            "data_id": self.data_id or self.generate_data_id()
        }
        if self.video_metadata:
            result["video_metadata"] = self.video_metadata
        if self.status:
            result["status"] = self.status
        return result

    def generate_data_id(self) -> str:
        data_string = f"{self.device_id}{self.timestamp.isoformat()}{json.dumps(self.sensors, sort_keys=True)}"
        return hashlib.sha256(data_string.encode()).hexdigest()[:16]

    def validate(self) -> Tuple[bool, List[str]]:
        errors = []
        if not self.device_id:
            errors.append("缺少device_id字段")
        if not self.sensors:
            errors.append("缺少sensors字段")

        validators = {
            'temperature': self._validate_temperature,
            'humidity': self._validate_humidity,
            'light': self._validate_light,
            'motion': self._validate_motion
        }

        for sensor_type, sensor_data in self.sensors.items():
            if sensor_type in validators:
                is_valid, message = validators[sensor_type](sensor_data)
                if not is_valid:
                    errors.append(f"{sensor_type}: {message}")

        return len(errors) == 0, errors

    def _validate_temperature(self, data: Dict[str, Any]) -> Tuple[bool, str]:
        try:
            t = TemperatureData(value=float(data.get('value', 0)),
                                unit=data.get('unit', 'celsius'))
            return t.validate()
        except (ValueError, TypeError) as e:
            return False, f"温度数据格式错误: {str(e)}"

    def _validate_humidity(self, data: Dict[str, Any]) -> Tuple[bool, str]:
        try:
            h = HumidityData(value=float(data.get('value', 0)),
                             unit=data.get('unit', 'percent'))
            return h.validate()
        except (ValueError, TypeError) as e:
            return False, f"湿度数据格式错误: {str(e)}"

    def _validate_light(self, data: Dict[str, Any]) -> Tuple[bool, str]:
        try:
            l = LightData(value=float(data.get('value', 0)),
                          unit=data.get('unit', 'lux'))
            return l.validate()
        except (ValueError, TypeError) as e:
            return False, f"光照数据格式错误: {str(e)}"

    def _validate_motion(self, data: Dict[str, Any]) -> Tuple[bool, str]:
        try:
            m = MotionData(
                detected=bool(data.get('detected', False)),
                confidence=float(data.get('confidence', 0)),
                location=data.get('location')
            )
            return m.validate()
        except (ValueError, TypeError) as e:
            return False, f"运动数据格式错误: {str(e)}"

    def analyze_for_alerts(self) -> List[Dict[str, Any]]:
        """分析传感器数据，生成告警信息"""
        alerts = []
        cfg = Config()

        if 'temperature' in self.sensors:
            temp_value = self.sensors['temperature'].get('value')
            if temp_value:
                if temp_value < cfg.TEMPERATURE_ALERT_THRESHOLD['min']:
                    alerts.append({
                        "type": "temperature_low",
                        "message": f"温度过低: {temp_value}°C",
                        "severity": "warning",
                        "threshold": cfg.TEMPERATURE_ALERT_THRESHOLD['min'],
                        "value": temp_value
                    })
                elif temp_value > cfg.TEMPERATURE_ALERT_THRESHOLD['max']:
                    alerts.append({
                        "type": "temperature_high",
                        "message": f"温度过高: {temp_value}°C",
                        "severity": "warning",
                        "threshold": cfg.TEMPERATURE_ALERT_THRESHOLD['max'],
                        "value": temp_value
                    })

        if 'humidity' in self.sensors:
            humidity_value = self.sensors['humidity'].get('value')
            if humidity_value:
                if humidity_value < cfg.HUMIDITY_ALERT_THRESHOLD['min']:
                    alerts.append({
                        "type": "humidity_low",
                        "message": f"湿度过低: {humidity_value}%",
                        "severity": "warning",
                        "threshold": cfg.HUMIDITY_ALERT_THRESHOLD['min'],
                        "value": humidity_value
                    })
                elif humidity_value > cfg.HUMIDITY_ALERT_THRESHOLD['max']:
                    alerts.append({
                        "type": "humidity_high",
                        "message": f"湿度过高: {humidity_value}%",
                        "severity": "warning",
                        "threshold": cfg.HUMIDITY_ALERT_THRESHOLD['max'],
                        "value": humidity_value
                    })

        if 'motion' in self.sensors:
            motion_data = self.sensors['motion']
            if motion_data.get('detected', False) and motion_data.get('confidence', 0) > 0.8:
                alerts.append({
                    "type": "activity_detected",
                    "message": "检测到宠物活动",
                    "severity": "info",
                    "confidence": motion_data.get('confidence')
                })

        return alerts

    def get_summary(self) -> Dict[str, Any]:
        """获取传感器数据摘要"""
        summary = {
            "device_id": self.device_id,
            "timestamp": self.timestamp.isoformat(),
            "sensor_count": len(self.sensors),
            "has_video": self.video_metadata is not None,
            "has_status": self.status is not None,
            "alerts": self.analyze_for_alerts()
        }
        for sensor_type, sensor_data in self.sensors.items():
            if 'value' in sensor_data:
                summary[f"{sensor_type}_value"] = sensor_data['value']
                summary[f"{sensor_type}_unit"] = sensor_data.get('unit', '')
        return summary
