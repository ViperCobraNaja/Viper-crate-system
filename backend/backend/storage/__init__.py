"""存储层统一入口 —— DataStorageManager 门面"""

import logging
from typing import Dict, Any, List, Optional

from config import Config
from storage.file_storage import LocalStorage
from storage.db_storage import DatabaseStorage
from storage.video_storage import VideoFileStorage

logger = logging.getLogger(__name__)


class DataStorageManager:
    """数据存储管理器：协调本地文件存储、MongoDB 和视频文件存储"""

    def __init__(self, mongo_client=None):
        self.config = Config()
        self.local_storage = LocalStorage()
        self.video_storage = VideoFileStorage()
        self.db_storage = None

        if mongo_client:
            try:
                self.db_storage = DatabaseStorage(mongo_client)
                logger.info("存储管理器初始化完成（本地 + 数据库 + 视频）")
            except Exception as e:
                logger.warning(f"数据库存储初始化失败，仅使用本地存储: {e}")
                self.db_storage = None
        else:
            logger.info("存储管理器初始化完成（仅本地存储 + 视频文件）")

    # ============ 传感器数据 ============

    def save_sensor_data(self, data: Dict[str, Any]) -> Dict[str, Any]:
        result = {"local": None, "database": None, "success": False}
        try:
            result["local"] = self.local_storage.save_sensor_data(data)
            if self.db_storage:
                result["database"] = self.db_storage.save_sensor_data(data)
            result["success"] = True
        except Exception as e:
            logger.error(f"保存传感器数据失败: {e}")
            result["error"] = str(e)
        return result

    def get_sensor_history(self, device_id: str = None,
                           start_time: str = None, end_time: str = None,
                           limit: int = 100) -> List[Dict[str, Any]]:
        return self.local_storage.get_sensor_history(
            device_id, start_time, end_time, limit)

    # ============ 视频元数据 ============

    def save_video_metadata(self, metadata: Dict[str, Any]) -> Dict[str, Any]:
        result = {"local": None, "database": None, "success": False}
        try:
            result["local"] = self.local_storage.save_video_metadata(metadata)
            if self.db_storage:
                result["database"] = self.db_storage.save_video_metadata(metadata)
            result["success"] = True
        except Exception as e:
            logger.error(f"保存视频元数据失败: {e}")
            result["error"] = str(e)
        return result

    # ============ 设备状态 ============

    def update_device_status(self, status_data: Dict[str, Any]) -> bool:
        if self.db_storage:
            return self.db_storage.update_device_status(status_data)
        return False

    def get_device_status(self, device_id: str) -> Optional[Dict[str, Any]]:
        if self.db_storage:
            return self.db_storage.get_device_status(device_id)
        return None

    # ============ 运动事件 ============

    def save_motion_event(self, event: dict) -> Optional[str]:
        if self.db_storage:
            return self.db_storage.save_motion_event(event)
        return None

    def query_motion_events(self, device_id: str = None,
                            start_time=None, end_time=None,
                            limit: int = 100) -> list:
        if self.db_storage:
            return self.db_storage.query_motion_events(
                device_id, start_time, end_time, limit)
        return []

    # ============ 设备命令 ============

    def enqueue_command(self, device_id: str, command: str,
                        params: dict = None) -> Optional[str]:
        if self.db_storage:
            return self.db_storage.enqueue_command(device_id, command, params)
        return None

    def poll_commands(self, device_id: str) -> list:
        if self.db_storage:
            return self.db_storage.poll_commands(device_id)
        return []

    def mark_command_executed(self, command_id: str, success: bool):
        if self.db_storage:
            self.db_storage.mark_command_executed(command_id, success)

    # ============ 设备心跳 ============

    def save_heartbeat(self, heartbeat: dict) -> Optional[str]:
        if self.db_storage:
            return self.db_storage.save_heartbeat(heartbeat)
        return None

    def get_offline_devices(self, threshold_seconds: int = 600) -> list:
        if self.db_storage:
            return self.db_storage.get_offline_devices(threshold_seconds)
        return []

    # ============ 统计 ============

    def get_sensor_stats(self, device_id: str = None, days: int = 7) -> dict:
        if self.db_storage:
            return self.db_storage.get_sensor_stats(device_id, days)
        return {}

    # ============ 维护 ============

    def cleanup(self):
        self.local_storage.cleanup_old_data()
        self.video_storage.cleanup_old_videos()

    def backup(self, backup_name: str = None) -> str:
        return self.local_storage.backup_data(backup_name)
