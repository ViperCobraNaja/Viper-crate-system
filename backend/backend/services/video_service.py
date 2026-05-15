"""视频服务 —— 上传接收、H.264→MP4 转换、查询删除"""

import os
import logging
from typing import Optional

from models.video import VideoMetadata, VideoUploadResult
from storage import DataStorageManager
from storage.video_storage import VideoFileStorage
from utils.video_utils import h264_to_mp4

logger = logging.getLogger(__name__)


class VideoService:
    """视频业务逻辑"""

    def __init__(self, storage: DataStorageManager):
        self.storage = storage
        self.video_fs = storage.video_storage
        logger.info("视频服务初始化完成")

    def receive_upload(self, device_id: str, file_data: bytes,
                       filename: str, metadata_dict: dict) -> VideoUploadResult:
        """接收视频文件上传：保存原始 H.264 → 转换为 MP4 → 存储元数据"""
        # 保存原始 H.264 文件
        video_id, h264_path = self.video_fs.save_raw_h264(
            device_id, file_data, filename)

        # 构建元数据
        metadata = VideoMetadata.from_dict(metadata_dict)
        metadata.video_id = video_id
        metadata.filename = filename
        metadata.device_id = device_id
        metadata.file_size = len(file_data)

        # 尝试转 MP4
        mp4_path = ""
        converted = False
        try:
            mp4_dir = os.path.dirname(h264_path)
            mp4_filename = f"{video_id}.mp4"
            mp4_path = os.path.join(mp4_dir, mp4_filename)
            converted = h264_to_mp4(h264_path, mp4_path, fps=metadata.fps or 20)
            if converted:
                logger.info(f"H.264→MP4 转换成功: {mp4_path}")
            else:
                logger.warning(f"H.264→MP4 转换失败，保留原始文件")
                mp4_path = h264_path  # 回退
        except Exception as e:
            logger.error(f"H.264→MP4 转换异常: {e}")
            mp4_path = h264_path

        # 保存元数据到数据库
        meta_dict = metadata.to_dict()
        self.storage.save_video_metadata(meta_dict)

        return VideoUploadResult(
            video_id=video_id,
            original_filename=filename,
            stored_path=h264_path,
            mp4_path=mp4_path,
            metadata=metadata,
            converted=converted,
        )

    def get_video_path(self, device_id: str, video_id: str) -> Optional[str]:
        """获取视频文件路径"""
        return self.video_fs.get_video_path(device_id, video_id)

    def delete_video(self, device_id: str, video_id: str) -> bool:
        """删除视频文件"""
        return self.video_fs.delete_video(device_id, video_id)

    def get_storage_usage(self, device_id: str = None) -> dict:
        """获取视频存储使用情况"""
        return self.video_fs.get_storage_usage(device_id)
