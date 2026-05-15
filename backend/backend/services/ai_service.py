"""AI 处理服务 —— 从 ai_processing.py 迁移业务逻辑"""

import json
import time
import logging
from datetime import datetime, timezone
from typing import Dict, Any, Union
import numpy as np

from config import Config
from llm_service import LLMService
from llm_models import RealPetDetector, RealActivityAnalyzer, LLMReportGenerator

logger = logging.getLogger(__name__)


class AIService:
    """AI 处理流水线服务"""

    def __init__(self, config: Config, llm_service: LLMService):
        self.config = config
        self.llm_service = llm_service

        self.pet_detector = RealPetDetector(config, llm_service)
        self.activity_analyzer = RealActivityAnalyzer(config, llm_service)
        self.report_generator = LLMReportGenerator(config, llm_service)

        self.stats = {
            "frames_processed": 0,
            "pets_detected": 0,
            "activities_detected": 0,
            "last_processed": None
        }

        logger.info("AI 服务初始化完成")

    def process_video_frame(self, frame_data: Union[np.ndarray, bytes],
                            sensor_data: dict = None) -> dict:
        start_time = time.time()
        try:
            detection = self.pet_detector.detect(frame_data)

            if sensor_data:
                activities = self.activity_analyzer.analyze_sensor_data(sensor_data)
            else:
                activities = self.activity_analyzer.analyze_video_detections(
                    [detection])

            self.stats["frames_processed"] += 1
            if detection.confidence > self.config.PET_DETECTION_CONFIDENCE_THRESHOLD:
                self.stats["pets_detected"] += 1
            self.stats["activities_detected"] += len(activities)
            self.stats["last_processed"] = datetime.now(timezone.utc).isoformat()

            processing_time = round(time.time() - start_time, 3)

            return {
                "success": True,
                "processing_time": processing_time,
                "detection": detection.to_dict(),
                "activities": [a.to_dict() for a in activities],
                "stats": self.get_stats(),
                "timestamp": datetime.now(timezone.utc).isoformat()
            }
        except Exception as e:
            logger.error(f"处理视频帧失败: {e}")
            return {
                "success": False,
                "error": str(e),
                "timestamp": datetime.now(timezone.utc).isoformat()
            }

    def process_sensor_data(self, sensor_data: dict) -> dict:
        try:
            activities = self.activity_analyzer.analyze_sensor_data(sensor_data)
            self.stats["activities_detected"] += len(activities)
            self.stats["last_processed"] = datetime.now(timezone.utc).isoformat()

            return {
                "success": True,
                "activities": [a.to_dict() for a in activities],
                "activity_summary": self.activity_analyzer.get_activity_summary(),
                "timestamp": datetime.now(timezone.utc).isoformat()
            }
        except Exception as e:
            logger.error(f"处理传感器数据失败: {e}")
            return {
                "success": False,
                "error": str(e),
                "timestamp": datetime.now(timezone.utc).isoformat()
            }

    def get_stats(self) -> dict:
        return self.stats.copy()

    def reset_stats(self):
        self.stats = {
            "frames_processed": 0,
            "pets_detected": 0,
            "activities_detected": 0,
            "last_processed": None
        }
        logger.info("AI 统计已重置")
