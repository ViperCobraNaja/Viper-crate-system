"""视频数据模型 —— 对应固件 video_recorder.h video_metadata_t"""

from datetime import datetime, timezone
from typing import Optional
from dataclasses import dataclass, field
import uuid


@dataclass
class VideoMetadata:
    """录像文件元数据，对应固件 video_metadata_t"""
    filename: str = ""
    create_time: str = ""       # ISO8601
    duration_ms: int = 0
    width: int = 0
    height: int = 0
    fps: int = 0
    bitrate: int = 0
    frame_count: int = 0
    avg_motion_level: float = 0.0
    file_size: int = 0
    device_id: str = ""
    video_id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])

    @classmethod
    def from_dict(cls, d: dict) -> 'VideoMetadata':
        return cls(
            filename=d.get('filename', ''),
            create_time=d.get('create_time', ''),
            duration_ms=d.get('duration_ms', 0),
            width=d.get('width', 0),
            height=d.get('height', 0),
            fps=d.get('fps', 0),
            bitrate=d.get('bitrate', 0),
            frame_count=d.get('frame_count', 0),
            avg_motion_level=d.get('avg_motion_level', 0.0),
            file_size=d.get('file_size', 0),
            device_id=d.get('device_id', ''),
            video_id=d.get('video_id', uuid.uuid4().hex[:12]),
        )

    def to_dict(self) -> dict:
        return {
            'video_id': self.video_id,
            'filename': self.filename,
            'create_time': self.create_time,
            'duration_ms': self.duration_ms,
            'width': self.width,
            'height': self.height,
            'fps': self.fps,
            'bitrate': self.bitrate,
            'frame_count': self.frame_count,
            'avg_motion_level': self.avg_motion_level,
            'file_size': self.file_size,
            'device_id': self.device_id,
        }


@dataclass
class VideoUploadResult:
    """视频上传处理结果"""
    video_id: str
    original_filename: str
    stored_path: str       # 服务端存储的原始 H.264 路径
    mp4_path: str          # 转换后的 MP4 路径（如果转换成功）
    metadata: VideoMetadata
    converted: bool = False
