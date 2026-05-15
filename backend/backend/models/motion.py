"""运动检测数据模型 —— 对应固件 motion_detector.h"""

from datetime import datetime, timezone
from typing import Optional, List
from dataclasses import dataclass, field

MOTION_GRID_COLS = 8
MOTION_GRID_ROWS = 6
MOTION_GRID_SIZE = MOTION_GRID_COLS * MOTION_GRID_ROWS  # 48


@dataclass
class MotionHeatmap:
    """运动热度图，对应固件 motion_heatmap_t"""
    grid_motion: List[int] = field(default_factory=lambda: [0] * MOTION_GRID_SIZE)
    grid_count: List[int] = field(default_factory=lambda: [0] * MOTION_GRID_SIZE)
    total_motion: int = 0
    total_valid: int = 0
    avg_motion: float = 0.0
    hot_zones: List[int] = field(default_factory=lambda: [0, 0, 0, 0])
    frame_timestamp: int = 0

    @classmethod
    def from_dict(cls, d: dict) -> 'MotionHeatmap':
        return cls(
            grid_motion=d.get('grid_motion', [0] * MOTION_GRID_SIZE),
            grid_count=d.get('grid_count', [0] * MOTION_GRID_SIZE),
            total_motion=d.get('total_motion', 0),
            total_valid=d.get('total_valid', 0),
            avg_motion=d.get('avg_motion', 0.0),
            hot_zones=d.get('hot_zones', [0, 0, 0, 0]),
            frame_timestamp=d.get('frame_timestamp', 0),
        )

    def to_dict(self) -> dict:
        return {
            'grid_motion': self.grid_motion,
            'grid_count': self.grid_count,
            'total_motion': self.total_motion,
            'total_valid': self.total_valid,
            'avg_motion': self.avg_motion,
            'hot_zones': self.hot_zones,
            'frame_timestamp': self.frame_timestamp,
        }


@dataclass
class MotionEvent:
    """运动检测事件，对应固件 cloud_interface_report_motion()"""
    device_id: str
    timestamp: datetime
    motion_level: int  # 0-100
    heatmap: MotionHeatmap
    snapshot_base64: Optional[str] = None

    @classmethod
    def from_dict(cls, d: dict) -> 'MotionEvent':
        ts = d.get('timestamp')
        if isinstance(ts, (int, float)):
            timestamp = datetime.fromtimestamp(ts / 1000, tz=timezone.utc)
        elif isinstance(ts, str):
            timestamp = datetime.fromisoformat(ts.replace('Z', '+00:00'))
        else:
            timestamp = datetime.now(timezone.utc)

        heatmap = MotionHeatmap.from_dict(d.get('heatmap', {}))

        return cls(
            device_id=d.get('device_id', 'unknown'),
            timestamp=timestamp,
            motion_level=int(d.get('motion_level', 0)),
            heatmap=heatmap,
            snapshot_base64=d.get('snapshot'),
        )

    def to_dict(self) -> dict:
        return {
            'device_id': self.device_id,
            'timestamp': self.timestamp.isoformat(),
            'motion_level': self.motion_level,
            'heatmap': self.heatmap.to_dict(),
            'snapshot': self.snapshot_base64,
        }
