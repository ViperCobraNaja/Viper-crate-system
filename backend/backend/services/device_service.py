"""设备管理服务 —— 心跳、命令队列、在线状态"""

import logging
from datetime import datetime, timezone
from typing import List, Optional

from models.device import DeviceHeartbeat, DeviceCommand, DeviceStatus
from storage import DataStorageManager

logger = logging.getLogger(__name__)


class DeviceService:
    """设备生命周期管理"""

    def __init__(self, storage: DataStorageManager):
        self.storage = storage
        self.config = storage.config
        logger.info("设备管理服务初始化完成")

    # ---- 心跳 ----

    def process_heartbeat(self, data: dict) -> DeviceHeartbeat:
        """处理设备心跳上报"""
        heartbeat = DeviceHeartbeat.from_dict(data)
        self.storage.save_heartbeat(heartbeat.to_dict())
        logger.debug(f"心跳: {heartbeat.device_id} online, "
                     f"CPU={heartbeat.cpu_usage}% MEM={heartbeat.memory_usage}%")
        return heartbeat

    def check_offline_devices(self) -> List[str]:
        """检测离线设备并返回 ID 列表"""
        threshold = self.config.DEVICE_OFFLINE_THRESHOLD
        return self.storage.get_offline_devices(threshold)

    # ---- 命令队列 ----

    def enqueue_command(self, device_id: str, command: str,
                        params: dict = None) -> Optional[str]:
        """向设备下发命令"""
        valid_commands = ('start_recording', 'stop_recording', 'reboot')
        if command not in valid_commands:
            logger.warning(f"无效命令: {command}")
            return None
        cmd_id = self.storage.enqueue_command(device_id, command, params)
        if cmd_id:
            logger.info(f"命令已下发: {device_id} -> {command}")
        return cmd_id

    def poll_commands(self, device_id: str) -> list:
        """设备轮询待执行命令"""
        return self.storage.poll_commands(device_id)

    def mark_command_done(self, command_id: str, success: bool):
        """标记命令执行结果"""
        self.storage.mark_command_executed(command_id, success)

    # ---- 状态 ----

    def get_device_info(self, device_id: str) -> DeviceStatus:
        """获取设备综合信息"""
        raw = self.storage.get_device_status(device_id)
        if raw:
            last_hb = raw.get('last_heartbeat')
            if isinstance(last_hb, str):
                try:
                    last_hb = datetime.fromisoformat(
                        last_hb.replace('Z', '+00:00'))
                except ValueError:
                    last_hb = None

            threshold = self.config.DEVICE_OFFLINE_THRESHOLD
            is_online = raw.get('online', False)
            if is_online and last_hb:
                elapsed = (datetime.now(timezone.utc) - last_hb).total_seconds()
                is_online = elapsed < threshold

            pending = len(self.storage.poll_commands(device_id))

            return DeviceStatus(
                device_id=device_id,
                online=is_online,
                last_heartbeat=last_hb,
                last_cpu_usage=raw.get('last_cpu_usage', 0),
                last_memory_usage=raw.get('last_memory_usage', 0),
                last_storage_usage=raw.get('last_storage_usage', 0),
                last_temperature=raw.get('last_temperature', 0.0),
                last_uptime_ms=raw.get('last_uptime_ms', 0),
                pending_commands=pending,
            )

        return DeviceStatus(device_id=device_id)
