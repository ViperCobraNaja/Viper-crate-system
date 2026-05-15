"""
宠语者 - LLM模型实现
实现真实的宠物检测、活动分析和报告生成功能
"""

import json
import time
import logging
import re
from datetime import datetime, timezone, timedelta
from typing import Dict, Any, List, Tuple, Optional, Union
from enum import Enum
from dataclasses import dataclass
from typing import cast

import numpy as np

from config import Config
from llm_service import LLMService, LLMError
from utils.image_utils import image_to_base64, prepare_image_for_qwen, ImageProcessingError

logger = logging.getLogger(__name__)


class PetType(Enum):
    """宠物类型枚举"""
    UNKNOWN = "unknown"
    CAT = "cat"
    DOG = "dog"
    BIRD = "bird"
    RABBIT = "rabbit"


class ActivityType(Enum):
    """活动类型枚举"""
    RESTING = "resting"          # 休息
    EATING = "eating"            # 进食
    DRINKING = "drinking"        # 喝水
    PLAYING = "playing"          # 玩耍
    EXPLORING = "exploring"      # 探索
    SCRATCHING = "scratching"    # 抓挠
    VOCALIZING = "vocalizing"    # 发声


@dataclass
class DetectionResult:
    """检测结果"""
    pet_type: PetType
    confidence: float
    bounding_box: Optional[Tuple[int, int, int, int]] = None  # (x, y, width, height)
    timestamp: Optional[datetime] = None
    frame_id: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        """转换为字典"""
        result = {
            "pet_type": self.pet_type.value,
            "confidence": self.confidence,
            "timestamp": self.timestamp.isoformat() if self.timestamp else None
        }
        if self.bounding_box:
            result["bounding_box"] = {
                "x": self.bounding_box[0],
                "y": self.bounding_box[1],
                "width": self.bounding_box[2],
                "height": self.bounding_box[3]
            }
        if self.frame_id:
            result["frame_id"] = self.frame_id
        return result


@dataclass
class ActivityResult:
    """活动识别结果"""
    activity_type: ActivityType
    confidence: float
    duration: float  # 活动持续时间（秒）
    timestamp: Optional[datetime] = None
    pet_type: Optional[PetType] = None

    def to_dict(self) -> Dict[str, Any]:
        """转换为字典"""
        result = {
            "activity_type": self.activity_type.value,
            "confidence": self.confidence,
            "duration": self.duration,
            "timestamp": self.timestamp.isoformat() if self.timestamp else None
        }
        if self.pet_type:
            result["pet_type"] = self.pet_type.value
        return result


class RealPetDetector:
    """真实的宠物检测器 - 使用Qwen-VL-Plus API"""

    def __init__(self, config: Config, llm_service: LLMService):
        """
        初始化宠物检测器

        Args:
            config: 配置对象
            llm_service: LLM服务实例
        """
        self.config = config
        self.llm_service = llm_service
        self.model_name = 'qwen_vl_plus'
        logger.info("LLM宠物检测器初始化完成")

    def detect(self, frame_data: Union[np.ndarray, bytes, str], frame_id: str = None) -> DetectionResult:
        """
        使用LLM进行宠物检测

        Args:
            frame_data: 图像帧数据
            frame_id: 帧ID

        Returns:
            检测结果
        """
        try:
            logger.debug(f"开始宠物检测，帧ID: {frame_id}")

            # 准备输入数据
            input_data = {
                'image_data': frame_data
            }

            # 调用LLM模型
            result = self.llm_service.call_model(
                task_name='pet_detection',
                input_data=input_data,
                model_name=self.model_name
            )

            # 解析结果
            if not result.get('success'):
                raise LLMError(f"LLM检测失败: {result.get('error', '未知错误')}")

            # 获取LLM返回的结果
            lmm_result = result.get('result', {})
            logger.debug(f"LLM宠物检测结果: {lmm_result}")

            # 解析检测结果
            pet_type_str = lmm_result.get('pet_type', 'unknown').lower()
            pet_type = self._parse_pet_type(pet_type_str)

            confidence = lmm_result.get('confidence', 0.0)
            if not isinstance(confidence, (int, float)):
                confidence = 0.0

            bounding_box = None
            bbox_data = lmm_result.get('bounding_box')
            if bbox_data and isinstance(bbox_data, dict):
                try:
                    bounding_box = (
                        int(bbox_data.get('x', 0)),
                        int(bbox_data.get('y', 0)),
                        int(bbox_data.get('width', 0)),
                        int(bbox_data.get('height', 0))
                    )
                except (ValueError, TypeError):
                    logger.warning("边界框数据格式错误，忽略边界框")
                    bounding_box = None

            # 构造返回结果
            detection_result = DetectionResult(
                pet_type=pet_type,
                confidence=float(confidence),
                bounding_box=bounding_box,
                timestamp=datetime.now(timezone.utc),
                frame_id=frame_id or self._generate_frame_id(frame_data)
            )

            logger.debug(f"宠物检测完成: {pet_type.value} (置信度: {confidence})")
            return detection_result

        except Exception as e:
            logger.error(f"宠物检测失败: {str(e)}")
            # 降级到默认值
            default_result = DetectionResult(
                pet_type=PetType.UNKNOWN,
                confidence=0.0,
                timestamp=datetime.now(timezone.utc),
                frame_id=frame_id or self._generate_frame_id(frame_data)
            )
            return default_result

    def _parse_pet_type(self, pet_type_str: str) -> PetType:
        """解析宠物类型字符串"""
        pet_type_str = pet_type_str.lower()

        if 'cat' in pet_type_str:
            return PetType.CAT
        elif 'dog' in pet_type_str:
            return PetType.DOG
        elif 'bird' in pet_type_str:
            return PetType.BIRD
        elif 'rabbit' in pet_type_str or '兔' in pet_type_str:
            return PetType.RABBIT
        else:
            return PetType.UNKNOWN

    def _generate_frame_id(self, frame_data: Union[np.ndarray, bytes, str]) -> str:
        """生成帧ID"""
        try:
            # 尝试从数据中生成哈希值
            if isinstance(frame_data, bytes):
                data_hash = hashlib.md5(frame_data).hexdigest()[:8]
            elif isinstance(frame_data, np.ndarray):
                # 如果是numpy数组，转换为字节
                data_bytes = frame_data.tobytes()
                data_hash = hashlib.md5(data_bytes).hexdigest()[:8]
            elif isinstance(frame_data, str):
                # 如果是字符串路径，使用路径的哈希
                data_hash = hashlib.md5(frame_data.encode()).hexdigest()[:8]
            else:
                # 其他类型使用类型和字符串表示
                data_hash = hashlib.md5(str(frame_data).encode()).hexdigest()[:8]

            return f"frame_{data_hash}"
        except Exception:
            # 如果生成失败，使用时间戳
            return f"frame_{int(time.time()) % 1000000}"


class RealActivityAnalyzer:
    """真实的活动分析器 - 使用DeepSeek Reasoner API"""

    def __init__(self, config: Config, llm_service: LLMService):
        """
        初始化活动分析器

        Args:
            config: 配置对象
            llm_service: LLM服务实例
        """
        self.config = config
        self.llm_service = llm_service
        self.model_name = 'deepseek_reasoner'
        self.activity_history = []
        logger.info("LLM活动分析器初始化完成")

    def analyze_sensor_data(self, sensor_data: Dict[str, Any]) -> List[ActivityResult]:
        """
        基于传感器数据分析宠物活动

        Args:
            sensor_data: 传感器数据

        Returns:
            活动识别结果列表
        """
        try:
            logger.debug("开始活动分析")

            # 准备输入数据
            # 提取传感器数据摘要
            sensors = sensor_data.get('sensors', {})
            motion_data = sensors.get('motion', {})
            light_data = sensors.get('light', {})
            temp_data = sensors.get('temperature', {})
            humidity_data = sensors.get('humidity', {})

            # 构造传感器摘要
            sensor_summary = {
                'motion': {
                    'detected': motion_data.get('detected', False),
                    'confidence': motion_data.get('confidence', 0),
                },
                'light': {
                    'value': light_data.get('value', 0),
                    'unit': light_data.get('unit', 'lux')
                },
                'temperature': {
                    'value': temp_data.get('value', 0),
                    'unit': temp_data.get('unit', 'celsius')
                },
                'humidity': {
                    'value': humidity_data.get('value', 0),
                    'unit': humidity_data.get('unit', 'percent')
                }
            }

            # 准备输入数据
            input_data = {
                'sensor_data_summary': json.dumps(sensor_summary, ensure_ascii=False),
                'timestamp': sensor_data.get('timestamp', '')
            }

            # 调用LLM模型
            result = self.llm_service.call_model(
                task_name='activity_recognition',
                input_data=input_data,
                model_name=self.model_name
            )

            # 解析结果
            if not result.get('success'):
                raise LLMError(f"LLM活动分析失败: {result.get('error', '未知错误')}")

            # 获取LLM返回的结果
            lmm_result = result.get('result', {})
            logger.debug(f"LLM活动分析结果: {lmm_result}")

            # 处理活动结果
            activities = []

            # 检查是否返回了多个活动
            if isinstance(lmm_result, list):
                # 多个活动的情况
                for activity_data in lmm_result:
                    activity = self._parse_activity_result(activity_data)
                    if activity:
                        activities.append(activity)
            else:
                # 单个活动的情况
                activity = self._parse_activity_result(lmm_result)
                if activity:
                    activities.append(activity)

            # 保存到历史记录
            self.activity_history.extend(activities)

            # 限制历史记录大小
            if len(self.activity_history) > 100:
                self.activity_history = self.activity_history[-100:]

            logger.debug(f"活动分析完成，识别到 {len(activities)} 个活动")
            return activities

        except Exception as e:
            logger.error(f"活动分析失败: {str(e)}")
            # 降级：返回默认活动
            default_activity = ActivityResult(
                activity_type=ActivityType.RESTING,
                confidence=0.7,
                duration=1.0,
                timestamp=datetime.now(timezone.utc)
            )
            return [default_activity]

    def _parse_activity_result(self, activity_data: Dict[str, Any]) -> Optional[ActivityResult]:
        """解析单个活动结果"""
        try:
            # 解析活动类型
            activity_type_str = activity_data.get('activity_type', 'resting').lower()
            activity_type = self._parse_activity_type(activity_type_str)

            # 解析置信度
            confidence = activity_data.get('confidence', 0.0)
            if not isinstance(confidence, (int, float)):
                confidence = 0.0

            # 解析持续时间
            duration = activity_data.get('duration', 0.0)
            if not isinstance(duration, (int, float)):
                duration = 0.0

            # 解析宠物类型
            pet_type = None
            pet_type_str = activity_data.get('pet_type')
            if pet_type_str:
                pet_type = self._parse_pet_type(pet_type_str.lower())

            return ActivityResult(
                activity_type=activity_type,
                confidence=float(confidence),
                duration=float(duration),
                timestamp=datetime.now(timezone.utc),
                pet_type=pet_type
            )

        except Exception as e:
            logger.error(f"解析活动结果失败: {str(e)}")
            return None

    def _parse_activity_type(self, activity_type_str: str) -> ActivityType:
        """解析活动类型字符串"""
        activity_type_str = activity_type_str.lower()

        if 'play' in activity_type_str or '玩' in activity_type_str:
            return ActivityType.PLAYING
        elif 'rest' in activity_type_str or '休息' in activity_type_str:
            return ActivityType.RESTING
        elif 'eat' in activity_type_str or '进食' in activity_type_str:
            return ActivityType.EATING
        elif 'drink' in activity_type_str or '喝水' in activity_type_str:
            return ActivityType.DRINKING
        elif 'explore' in activity_type_str or '探索' in activity_type_str:
            return ActivityType.EXPLORING
        elif 'scratch' in activity_type_str or '抓' in activity_type_str:
            return ActivityType.SCRATCHING
        elif 'vocal' in activity_type_str or '发声' in activity_type_str:
            return ActivityType.VOCALIZING
        else:
            return ActivityType.RESTING

    def _parse_pet_type(self, pet_type_str: str) -> PetType:
        """解析宠物类型字符串"""
        pet_type_str = pet_type_str.lower()

        if 'cat' in pet_type_str:
            return PetType.CAT
        elif 'dog' in pet_type_str:
            return PetType.DOG
        elif 'bird' in pet_type_str:
            return PetType.BIRD
        elif 'rabbit' in pet_type_str or '兔' in pet_type_str:
            return PetType.RABBIT
        else:
            return PetType.UNKNOWN

    def analyze_video_detections(self, detections: List[DetectionResult]) -> List[ActivityResult]:
        """
        基于视频检测结果分析活动

        Args:
            detections: 宠物检测结果列表

        Returns:
            活动识别结果
        """
        if not detections:
            return []

        # 简单的基于检测结果的分析
        activities = []
        detection_count = len(detections)

        if detection_count > 5:
            # 频繁检测到宠物，可能是活跃状态
            confidences = [d.confidence for d in detections]
            avg_confidence = sum(confidences) / len(confidences) if confidences else 0

            activity = ActivityResult(
                activity_type=ActivityType.EXPLORING,
                confidence=min(0.8, avg_confidence),
                duration=10.0,
                timestamp=datetime.now(timezone.utc)
            )
            activities.append(activity)

        return activities

    def get_activity_summary(self, time_window: int = 3600) -> Dict[str, Any]:
        """
        获取活动摘要

        Args:
            time_window: 时间窗口（秒）

        Returns:
            活动摘要
        """
        cutoff_time = datetime.now(timezone.utc) - timedelta(seconds=time_window)

        recent_activities = [
            activity for activity in self.activity_history
            if activity.timestamp and activity.timestamp > cutoff_time
        ]

        # 统计活动类型
        activity_counts = {}
        for activity in recent_activities:
            activity_type = activity.activity_type.value
            activity_counts[activity_type] = activity_counts.get(activity_type, 0) + 1

        # 计算总活动时间
        total_duration = sum(activity.duration for activity in recent_activities)

        # 计算平均置信度
        if recent_activities:
            avg_confidence = sum(activity.confidence for activity in recent_activities) / len(recent_activities)
        else:
            avg_confidence = 0

        return {
            "total_activities": len(recent_activities),
            "total_duration": total_duration,
            "average_confidence": round(avg_confidence, 2),
            "activity_counts": activity_counts,
            "time_window_seconds": time_window
        }


class LLMReportGenerator:
    """LLM日报生成器"""

    def __init__(self, config: Config, llm_service: LLMService):
        """
        初始化日报生成器

        Args:
            config: 配置对象
            llm_service: LLM服务实例
        """
        self.config = config
        self.llm_service = llm_service
        self.model_name = 'deepseek_reasoner'
        logger.info("LLM日报生成器初始化完成")

    def generate_daily_report(self, sensor_data: List[Dict[str, Any]],
                           activity_logs: List[Dict[str, Any]],
                           video_summary: Dict[str, Any],
                           device_id: str = None,
                           date: str = None) -> Dict[str, Any]:
        """
        使用LLM生成宠物日报

        Args:
            sensor_data: 传感器数据列表
            activity_logs: 活动记录列表
            video_summary: 视频分析摘要
            device_id: 设备ID
            date: 日期

        Returns:
            日报结果
        """
        try:
            logger.debug("开始生成日报")

            # 构造输入数据
            # 传感器统计数据
            sensor_stats = self._calculate_sensor_statistics(sensor_data)

            # 活动统计数据
            activity_summary = self._calculate_activity_summary(activity_logs)

            # 视频摘要
            video_analysis = video_summary or {}

            # 准备输入数据
            input_data = {
                'date': date or datetime.now().strftime('%Y-%m-%d'),
                'device_id': device_id or 'unknown',
                'sensor_stats': json.dumps(sensor_stats, ensure_ascii=False),
                'activity_logs': json.dumps(activity_logs, ensure_ascii=False),
                'video_summary': json.dumps(video_analysis, ensure_ascii=False)
            }

            # 调用LLM模型
            result = self.llm_service.call_model(
                task_name='daily_report',
                input_data=input_data,
                model_name=self.model_name
            )

            # 解析结果
            if not result.get('success'):
                raise LLMError(f"LLM日报生成失败: {result.get('error', '未知错误')}")

            # 获取LLM返回的结果
            lmm_result = result.get('result', {})
            logger.debug(f"LLM日报生成结果: {lmm_result}")

            # 返回完整报告
            report = {
                'date': lmm_result.get('date', date or datetime.now().strftime('%Y-%m-%d')),
                'device_id': lmm_result.get('device_id', device_id or 'unknown'),
                'summary': lmm_result.get('summary', {}),
                'detailed_analysis': lmm_result.get('detailed_analysis', {}),
                'health_assessment': lmm_result.get('health_assessment', {}),
                'recommendations': lmm_result.get('recommendations', {}),
                'fun_facts': lmm_result.get('fun_facts', []),
                'generated_at': datetime.now(timezone.utc).isoformat()
            }

            logger.debug("日报生成完成")
            return report

        except Exception as e:
            logger.error(f"日报生成失败: {str(e)}")
            # 降级：返回简单报告
            return self._generate_simple_report(sensor_data, activity_logs, video_summary, device_id, date)

    def _calculate_sensor_statistics(self, sensor_data: List[Dict[str, Any]]) -> Dict[str, Any]:
        """计算传感器统计数据"""
        if not sensor_data:
            return {
                'total_records': 0,
                'avg_temperature': 0,
                'avg_humidity': 0,
                'avg_light': 0,
                'motion_count': 0
            }

        # 统计各项数据
        temp_values = []
        humidity_values = []
        light_values = []
        motion_count = 0

        for record in sensor_data:
            sensors = record.get('sensors', {})

            temp = sensors.get('temperature', {}).get('value')
            if temp is not None:
                temp_values.append(float(temp))

            humidity = sensors.get('humidity', {}).get('value')
            if humidity is not None:
                humidity_values.append(float(humidity))

            light = sensors.get('light', {}).get('value')
            if light is not None:
                light_values.append(float(light))

            motion = sensors.get('motion', {}).get('detected', False)
            if motion:
                motion_count += 1

        # 计算平均值
        avg_temp = sum(temp_values) / len(temp_values) if temp_values else 0
        avg_humidity = sum(humidity_values) / len(humidity_values) if humidity_values else 0
        avg_light = sum(light_values) / len(light_values) if light_values else 0

        return {
            'total_records': len(sensor_data),
            'avg_temperature': round(avg_temp, 2),
            'avg_humidity': round(avg_humidity, 2),
            'avg_light': round(avg_light, 2),
            'motion_count': motion_count
        }

    def _calculate_activity_summary(self, activity_logs: List[Dict[str, Any]]) -> Dict[str, Any]:
        """计算活动统计数据"""
        if not activity_logs:
            return {
                'total_activities': 0,
                'activity_distribution': {},
                'total_duration': 0
            }

        # 统计活动分布
        activity_counts = {}
        total_duration = 0

        for log in activity_logs:
            activity_type = log.get('activity_type', 'resting')
            activity_counts[activity_type] = activity_counts.get(activity_type, 0) + 1
            duration = log.get('duration', 0)
            total_duration += duration

        return {
            'total_activities': len(activity_logs),
            'activity_distribution': activity_counts,
            'total_duration': round(total_duration, 2)
        }

    def _generate_simple_report(self, sensor_data: List[Dict[str, Any]],
                              activity_logs: List[Dict[str, Any]],
                              video_summary: Dict[str, Any],
                              device_id: str = None,
                              date: str = None) -> Dict[str, Any]:
        """生成简单的降级报告"""
        # 计算基本统计
        sensor_stats = self._calculate_sensor_statistics(sensor_data)
        activity_summary = self._calculate_activity_summary(activity_logs)

        # 简单的健康建议
        recommendations = []
        if sensor_stats['avg_temperature'] > 30:
            recommendations.append("当前温度偏高，建议加强通风或使用降温设备")
        elif sensor_stats['avg_temperature'] < 15:
            recommendations.append("当前温度偏低，建议适当保暖")

        if sensor_stats['avg_humidity'] < 40:
            recommendations.append("空气较干燥，建议增加湿度")
        elif sensor_stats['avg_humidity'] > 70:
            recommendations.append("空气较潮湿，建议加强通风除湿")

        if activity_summary['total_activities'] < 5:
            recommendations.append("宠物活动较少，建议增加互动游戏")

        return {
            'date': date or datetime.now().strftime('%Y-%m-%d'),
            'device_id': device_id or 'unknown',
            'summary': {
                'total_activities': activity_summary['total_activities'],
                'avg_temperature': sensor_stats['avg_temperature'],
                'avg_humidity': sensor_stats['avg_humidity'],
                'avg_light': sensor_stats['avg_light'],
                'motion_count': sensor_stats['motion_count']
            },
            'detailed_analysis': {
                'morning_activities': [],
                'afternoon_activities': [],
                'evening_activities': [],
                'notable_events': []
            },
            'health_assessment': {
                'overall_status': 'normal',
                'concerns': [],
                'positive_signs': []
            },
            'recommendations': {
                'immediate': recommendations,
                'long_term': ["建议定期监测宠物健康状况", "保持适宜的温湿度环境"]
            },
            'fun_facts': [],
            'generated_at': datetime.now(timezone.utc).isoformat()
        }


# 测试函数
if __name__ == '__main__':
    # 配置日志
    logging.basicConfig(level=logging.INFO)

    print("测试LLM模型实现...")

    # 创建测试配置
    test_config = Config()
    test_config.LLM_ENABLED = True
    test_config.DEEPSEEK_API_KEY = "sk-e5d53f0c2ef94139b85ec53fcd585d8b"
    test_config.QWEN_API_KEY = "sk-4dd08e56abce44d781e8ad0b24fd7978"

    try:
        # 创建LLM服务实例
        llm_service = LLMService(test_config)

        # 测试宠物检测器
        print("\n1. 测试宠物检测器:")
        pet_detector = RealPetDetector(test_config, llm_service)
        # 由于无法真正调用API，这里只测试初始化
        print("  宠物检测器初始化成功")

        # 测试活动分析器
        print("\n2. 测试活动分析器:")
        activity_analyzer = RealActivityAnalyzer(test_config, llm_service)
        print("  活动分析器初始化成功")

        # 测试日报生成器
        print("\n3. 测试日报生成器:")
        report_generator = LLMReportGenerator(test_config, llm_service)
        print("  日报生成器初始化成功")

        print("\n测试完成!")

    except Exception as e:
        print(f"测试过程中发生错误: {e}")
        import traceback
        traceback.print_exc()