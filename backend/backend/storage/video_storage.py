"""视频文件专用存储 —— 文件系统存储 H.264/MP4 文件"""

import os
import shutil
import logging
import uuid
from datetime import datetime
from typing import Optional, Tuple

from config import Config

logger = logging.getLogger(__name__)


class VideoFileStorage:
    """视频文件系统存储管理

    存储路径: data/videos/{device_id}/{date}/{video_id}.{ext}
    """

    def __init__(self):
        self.config = Config()
        self.base_dir = self.config.VIDEO_STORAGE_DIR
        os.makedirs(self.base_dir, exist_ok=True)
        logger.info(f"视频文件存储初始化: {self.base_dir}")

    def _get_device_dir(self, device_id: str) -> str:
        today = datetime.now().strftime('%Y%m%d')
        path = os.path.join(self.base_dir, device_id, today)
        os.makedirs(path, exist_ok=True)
        return path

    def save_raw_h264(self, device_id: str, file_data: bytes,
                      original_filename: str) -> Tuple[str, str]:
        """保存原始 H.264 文件，返回 (video_id, 文件路径)"""
        video_id = uuid.uuid4().hex[:12]
        device_dir = self._get_device_dir(device_id)
        filename = f"{video_id}.h264"
        filepath = os.path.join(device_dir, filename)

        with open(filepath, 'wb') as f:
            f.write(file_data)

        logger.info(f"H.264 文件已保存: {filepath} ({len(file_data)} bytes)")
        return video_id, filepath

    def save_mp4(self, device_id: str, video_id: str, mp4_data: bytes) -> str:
        """保存转换后的 MP4 文件，返回文件路径"""
        device_dir = self._get_device_dir(device_id)
        filename = f"{video_id}.mp4"
        filepath = os.path.join(device_dir, filename)

        with open(filepath, 'wb') as f:
            f.write(mp4_data)

        logger.info(f"MP4 文件已保存: {filepath}")
        return filepath

    def get_video_path(self, device_id: str, video_id: str,
                       prefer_mp4: bool = True) -> Optional[str]:
        """查找视频文件路径，优先返回 MP4 版本"""
        # 递归搜索所有日期子目录
        device_root = os.path.join(self.base_dir, device_id)
        if not os.path.isdir(device_root):
            return None

        for date_dir in sorted(os.listdir(device_root), reverse=True):
            date_path = os.path.join(device_root, date_dir)
            if not os.path.isdir(date_path):
                continue

            if prefer_mp4:
                mp4_path = os.path.join(date_path, f"{video_id}.mp4")
                if os.path.exists(mp4_path):
                    return mp4_path

            h264_path = os.path.join(date_path, f"{video_id}.h264")
            if os.path.exists(h264_path):
                return h264_path

        return None

    def delete_video(self, device_id: str, video_id: str) -> bool:
        """删除视频文件（含所有格式）"""
        device_root = os.path.join(self.base_dir, device_id)
        if not os.path.isdir(device_root):
            return False

        deleted = False
        for date_dir in os.listdir(device_root):
            date_path = os.path.join(device_root, date_dir)
            if not os.path.isdir(date_path):
                continue
            for ext in ('.h264', '.mp4'):
                fp = os.path.join(date_path, f"{video_id}{ext}")
                if os.path.exists(fp):
                    os.remove(fp)
                    deleted = True
                    logger.info(f"已删除视频文件: {fp}")

        return deleted

    def get_storage_usage(self, device_id: str = None) -> dict:
        """获取存储使用统计"""
        total_size = 0
        total_files = 0
        base = self.base_dir if not device_id else os.path.join(
            self.base_dir, device_id)

        if os.path.isdir(base):
            for root, dirs, files in os.walk(base):
                for fn in files:
                    fp = os.path.join(root, fn)
                    try:
                        total_size += os.path.getsize(fp)
                        total_files += 1
                    except OSError:
                        pass

        return {
            'total_files': total_files,
            'total_size_bytes': total_size,
            'total_size_mb': round(total_size / (1024 * 1024), 2),
        }

    def cleanup_old_videos(self, max_days: int = 30) -> int:
        """清理超过 max_days 天的视频文件，返回删除数量"""
        import time
        cutoff = time.time() - (max_days * 86400)
        deleted = 0

        if not os.path.isdir(self.base_dir):
            return 0

        for device_dir in os.listdir(self.base_dir):
            device_path = os.path.join(self.base_dir, device_dir)
            if not os.path.isdir(device_path):
                continue
            for date_dir in os.listdir(device_path):
                date_path = os.path.join(device_path, date_dir)
                if not os.path.isdir(date_path):
                    continue
                try:
                    dir_mtime = os.path.getmtime(date_path)
                    if dir_mtime < cutoff:
                        n_files = len(os.listdir(date_path))
                        shutil.rmtree(date_path)
                        deleted += n_files
                        logger.info(f"清理过期视频目录: {date_path}")
                except OSError as e:
                    logger.warning(f"清理视频目录失败 {date_path}: {e}")

        return deleted
