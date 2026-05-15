"""设备管理数据模型 —— 对应固件 cloud_interface.h 设备状态上报"""

from datetime import datetime, timezone
from typing import Optional, Dict, Any
from dataclasses import dataclass, field
import uuid


@dataclass
class DeviceHeartbeat:
    """设备心跳 + 遥测数据，对应固件 cloud_interface_report_status()"""
    device_id: str
    timestamp: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    cpu_usage: int = 0          # 0-100
    memory_usage: int = 0       # 0-100
    storage_usage: int = 0      # 0-100
    temperature: float = 0.0    # 芯片温度（摄氏度）
    uptime_ms: int = 0

    @classmethod
    def from_dict(cls, d: dict) -> 'DeviceHeartbeat':
        ts = d.get('timestamp')
        if isinstance(ts, (int, float)):
            timestamp = datetime.fromtimestamp(ts / 1000, tz=timezone.utc)
        elif isinstance(ts, str):
            timestamp = datetime.fromisoformat(ts.replace('Z', '+00:00'))
        else:
            timestamp = datetime.now(timezone.utc)

        return cls(
            device_id=d.get('device_id', 'unknown'),
            timestamp=timestamp,
            cpu_usage=int(d.get('cpu_usage', 0)),
            memory_usage=int(d.get('memory_usage', 0)),
            storage_usage=int(d.get('storage_usage', 0)),
            temperature=float(d.get('temperature', 0.0)),
            uptime_ms=int(d.get('uptime_ms', 0)),
        )

    def to_dict(self) -> dict:
        return {
            'device_id': self.device_id,
            'timestamp': self.timestamp.isoformat(),
            'cpu_usage': self.cpu_usage,
            'memory_usage': self.memory_usage,
            'storage_usage': self.storage_usage,
            'temperature': self.temperature,
            'uptime_ms': self.uptime_ms,
        }


@dataclass
class DeviceCommand:
    """设备命令，对应固件 cloud_interface_process_command()"""
    command_id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    device_id: str = ""
    command: str = ""       # "start_recording" | "stop_recording" | "reboot"
    params: Dict[str, Any] = field(default_factory=dict)
    status: str = "pending"  # "pending" | "executed" | "failed"
    created_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    executed_at: Optional[datetime] = None

    @classmethod
    def from_dict(cls, d: dict) -> 'DeviceCommand':
        return cls(
            command_id=d.get('command_id', uuid.uuid4().hex[:12]),
            device_id=d.get('device_id', ''),
            command=d.get('command', ''),
            params=d.get('params', {}),
            status=d.get('status', 'pending'),
            created_at=datetime.now(timezone.utc),
            executed_at=None,
        )

    def to_dict(self) -> dict:
        return {
            'command_id': self.command_id,
            'device_id': self.device_id,
            'command': self.command,
            'params': self.params,
            'status': self.status,
            'created_at': self.created_at.isoformat(),
            'executed_at': self.executed_at.isoformat() if self.executed_at else None,
        }


@dataclass
class DeviceStatus:
    """设备综合状态"""
    device_id: str
    online: bool = False
    last_heartbeat: Optional[datetime] = None
    last_cpu_usage: int = 0
    last_memory_usage: int = 0
    last_storage_usage: int = 0
    last_temperature: float = 0.0
    last_uptime_ms: int = 0
    battery: Optional[int] = None
    wifi_connected: bool = False
    pending_commands: int = 0

    def to_dict(self) -> dict:
        return {
            'device_id': self.device_id,
            'online': self.online,
            'last_heartbeat': self.last_heartbeat.isoformat() if self.last_heartbeat else None,
            'last_cpu_usage': self.last_cpu_usage,
            'last_memory_usage': self.last_memory_usage,
            'last_storage_usage': self.last_storage_usage,
            'last_temperature': self.last_temperature,
            'last_uptime_ms': self.last_uptime_ms,
            'battery': self.battery,
            'wifi_connected': self.wifi_connected,
            'pending_commands': self.pending_commands,
        }
