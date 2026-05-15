"""
宠语者 - AI处理模块
负责宠物检测和活动识别功能

现在使用LLM实现，替代原来的模拟实现
"""

import json
import time
import logging
import hashlib
from datetime import datetime, timezone, timedelta
from typing import Dict, Any, List, Tuple, Optional, Union
from enum import Enum
from dataclasses import dataclass
import numpy as np

from config import Config
from llm_service import LLMService
from llm_models import RealPetDetector, RealActivityAnalyzer, LLMReportGenerator

# 配置日志
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


# MockPetDetector 已被替换为 RealPetDetector，因此移除


# ActivityAnalyzer 已被替换为 RealActivityAnalyzer，因此移除


class AIProcessingPipeline:
    """AI处理流水线"""

    def __init__(self, config: Config, llm_service: LLMService):
        self.config = config
        self.llm_service = llm_service

        # 初始化组件
        self.pet_detector = RealPetDetector(config, llm_service)
        self.activity_analyzer = RealActivityAnalyzer(config, llm_service)
        self.report_generator = LLMReportGenerator(config, llm_service)

        # 处理统计
        self.stats = {
            "frames_processed": 0,
            "pets_detected": 0,
            "activities_detected": 0,
            "last_processed": None
        }

        logger.info("AI处理流水线初始化完成")

    def process_video_frame(self, frame_data: Union[np.ndarray, bytes],
                           sensor_data: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        """
        处理视频帧

        Args:
            frame_data: 视频帧数据
            sensor_data: 传感器数据（可选）

        Returns:
            处理结果
        """
        start_time = time.time()

        try:
            # 宠物检测
            detection_result = self.pet_detector.detect(frame_data)

            # 活动识别
            activity_results = []
            if sensor_data:
                activity_results = self.activity_analyzer.analyze_sensor_data(sensor_data)
            else:
                # 如果没有传感器数据，基于检测结果分析
                activity_results = self.activity_analyzer.analyze_video_detections([detection_result])

            # 更新统计
            self.stats["frames_processed"] += 1
            if detection_result.confidence > self.config.PET_DETECTION_CONFIDENCE_THRESHOLD:
                self.stats["pets_detected"] += 1
            self.stats["activities_detected"] += len(activity_results)
            self.stats["last_processed"] = datetime.now(timezone.utc).isoformat()

            processing_time = round(time.time() - start_time, 3)

            # 构建结果
            result = {
                "success": True,
                "processing_time": round(processing_time, 3),
                "detection": detection_result.to_dict(),
                "activities": [activity.to_dict() for activity in activity_results],
                "stats": self.get_stats(),
                "timestamp": datetime.now(timezone.utc).isoformat()
            }

            logger.debug(f"视频帧处理完成: {processing_time:.3f}秒")
            return result

        except Exception as e:
            logger.error(f"处理视频帧失败: {str(e)}")
            return {
                "success": False,
                "error": str(e),
                "timestamp": datetime.now(timezone.utc).isoformat()
            }

    def process_sensor_data(self, sensor_data: Dict[str, Any]) -> Dict[str, Any]:
        """
        处理传感器数据（仅活动识别）

        Args:
            sensor_data: 传感器数据

        Returns:
            处理结果
        """
        try:
            activity_results = self.activity_analyzer.analyze_sensor_data(sensor_data)

            # 更新统计
            self.stats["activities_detected"] += len(activity_results)
            self.stats["last_processed"] = datetime.now(timezone.utc).isoformat()

            result = {
                "success": True,
                "activities": [activity.to_dict() for activity in activity_results],
                "activity_summary": self.activity_analyzer.get_activity_summary(),
                "timestamp": datetime.now(timezone.utc).isoformat()
            }

            logger.debug(f"传感器数据处理完成: 识别到{len(activity_results)}个活动")
            return result

        except Exception as e:
            logger.error(f"处理传感器数据失败: {str(e)}")
            return {
                "success": False,
                "error": str(e),
                "timestamp": datetime.now(timezone.utc).isoformat()
            }

    def get_stats(self) -> Dict[str, Any]:
        """获取处理统计"""
        return self.stats.copy()

    def reset_stats(self):
        """重置统计"""
        self.stats = {
            "frames_processed": 0,
            "pets_detected": 0,
            "activities_detected": 0,
            "last_processed": None
        }
        logger.info("AI处理统计已重置")


# AI处理器工厂类
class AIProcessorFactory:
    """AI处理器工厂"""

    @staticmethod
    def create_processor(config: Config, llm_service: LLMService = None) -> AIProcessingPipeline:
        """
        创建AI处理器

        Args:
            config: 配置对象
            llm_service: LLM服务实例（可选）

        Returns:
            AI处理流水线
        """
        return AIProcessingPipeline(config, llm_service)


# 测试函数
if __name__ == '__main__':
    # 配置日志
    logging.basicConfig(level=logging.INFO)

    # 创建配置
    config = Config()

    # 创建AI处理器
    print("测试AI处理模块...")
    processor = AIProcessorFactory.create_processor(config)

    # 测试宠物检测
    print("\n1. 测试宠物检测:")
    mock_frame = np.random.rand(480, 640, 3) * 255  # 模拟图像帧
    result = processor.process_video_frame(mock_frame)
    print(f"检测结果: {json.dumps(result, indent=2, ensure_ascii=False)}")

    # 测试传感器数据分析
    print("\n2. 测试传感器数据分析:")
    sensor_data = {
        "device_id": "ESP32-P4-001",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "sensors": {
            "motion": {"detected": True, "confidence": 0.85},
            "light": {"value": 600, "unit": "lux"}
        }
    }
    result = processor.process_sensor_data(sensor_data)
    print(f"活动识别结果: {json.dumps(result, indent=2, ensure_ascii=False)}")

    # 测试获取统计
    print("\n3. 测试获取统计:")
    stats = processor.get_stats()
    print(f"处理统计: {json.dumps(stats, indent=2, ensure_ascii=False)}")

    # 测试活动摘要
    print("\n4. 测试活动摘要:")
    summary = processor.activity_analyzer.get_activity_summary()
    print(f"活动摘要: {json.dumps(summary, indent=2, ensure_ascii=False)}")

    print("\n测试完成")