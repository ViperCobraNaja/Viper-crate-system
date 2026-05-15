"""本地文件存储 —— 处理 JSON 数据的 CRUD"""

import os
import json
import shutil
import logging
from datetime import datetime, timezone, timedelta
from typing import Dict, Any, List, Optional

from config import Config

logger = logging.getLogger(__name__)


class LocalStorage:
    """本地文件存储管理（JSON 文本数据）"""

    def __init__(self):
        self.config = Config()
        self._ensure_directories()
        logger.info("本地存储初始化完成")

    def _ensure_directories(self):
        dirs = [
            self.config.DATA_DIR,
            self.config.SENSOR_DATA_DIR,
            self.config.VIDEO_DATA_DIR,
            self.config.BACKUP_DIR,
            self.config.LOG_DIR,
        ]
        for d in dirs:
            os.makedirs(d, exist_ok=True)

    def save_sensor_data(self, data: Dict[str, Any]) -> str:
        try:
            device_id = data.get('device_id', 'unknown')
            timestamp = data.get('timestamp', datetime.now(timezone.utc).isoformat())
            if isinstance(timestamp, str):
                dt = datetime.fromisoformat(timestamp.replace('Z', '+00:00'))
            else:
                dt = timestamp

            filename = f"{device_id}_{dt.strftime('%Y%m%d_%H%M%S')}.json"
            filepath = os.path.join(self.config.SENSOR_DATA_DIR, filename)

            data['_stored_at'] = datetime.now(timezone.utc).isoformat()
            data['_storage_type'] = 'local_file'

            with open(filepath, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, ensure_ascii=False, default=str)

            logger.info(f"传感器数据保存到本地: {filepath}")
            return filepath
        except Exception as e:
            logger.error(f"保存传感器数据失败: {e}")
            raise

    def save_video_metadata(self, metadata: Dict[str, Any]) -> str:
        try:
            video_id = metadata.get('video_id', 'unknown')
            ts = metadata.get('timestamp', datetime.now(timezone.utc).isoformat())
            if isinstance(ts, str):
                dt = datetime.fromisoformat(ts.replace('Z', '+00:00'))
            else:
                dt = ts

            filename = f"video_{video_id}_{dt.strftime('%Y%m%d_%H%M%S')}.json"
            filepath = os.path.join(self.config.VIDEO_DATA_DIR, filename)

            metadata['_stored_at'] = datetime.now(timezone.utc).isoformat()
            metadata['_storage_type'] = 'video_metadata'

            with open(filepath, 'w', encoding='utf-8') as f:
                json.dump(metadata, f, indent=2, ensure_ascii=False, default=str)

            logger.info(f"视频元数据保存到本地: {filepath}")
            return filepath
        except Exception as e:
            logger.error(f"保存视频元数据失败: {e}")
            raise

    def get_sensor_history(self, device_id: Optional[str] = None,
                           start_time: Optional[str] = None,
                           end_time: Optional[str] = None,
                           limit: int = 100) -> List[Dict[str, Any]]:
        try:
            files = os.listdir(self.config.SENSOR_DATA_DIR)
            files = [f for f in files if f.endswith('.json')]
            files.sort(reverse=True)

            results = []
            for filename in files:
                if len(results) >= limit:
                    break
                filepath = os.path.join(self.config.SENSOR_DATA_DIR, filename)
                try:
                    with open(filepath, 'r', encoding='utf-8') as f:
                        data = json.load(f)
                    if device_id and data.get('device_id') != device_id:
                        continue
                    ts_str = data.get('timestamp')
                    if ts_str:
                        try:
                            data_time = datetime.fromisoformat(
                                ts_str.replace('Z', '+00:00'))
                        except ValueError:
                            continue
                        if start_time:
                            start_dt = datetime.fromisoformat(
                                start_time.replace('Z', '+00:00'))
                            if data_time < start_dt:
                                continue
                        if end_time:
                            end_dt = datetime.fromisoformat(
                                end_time.replace('Z', '+00:00'))
                            if data_time > end_dt:
                                continue
                    results.append(data)
                except (json.JSONDecodeError, OSError) as e:
                    logger.warning(f"读取文件失败 {filepath}: {e}")
                    continue

            logger.info(f"从本地加载{len(results)}条传感器数据")
            return results
        except Exception as e:
            logger.error(f"获取传感器历史失败: {e}")
            return []

    def cleanup_old_data(self):
        try:
            cutoff = datetime.now(timezone.utc) - timedelta(
                days=self.config.SENSOR_DATA_RETENTION_DAYS)

            sensor_files = os.listdir(self.config.SENSOR_DATA_DIR)
            deleted = 0
            for fn in sensor_files:
                if fn.endswith('.json'):
                    fp = os.path.join(self.config.SENSOR_DATA_DIR, fn)
                    try:
                        parts = fn.split('_')
                        if len(parts) >= 3:
                            date_str = parts[-2]
                            time_str = parts[-1].replace('.json', '')
                            try:
                                ft = datetime.strptime(
                                    f"{date_str}_{time_str}", "%Y%m%d_%H%M%S")
                                if ft < cutoff:
                                    os.remove(fp)
                                    deleted += 1
                            except ValueError:
                                pass
                    except OSError as e:
                        logger.warning(f"删除文件失败 {fp}: {e}")

            video_files = os.listdir(self.config.VIDEO_DATA_DIR)
            vd = 0
            for fn in video_files:
                if fn.endswith('.json'):
                    fp = os.path.join(self.config.VIDEO_DATA_DIR, fn)
                    try:
                        mtime = os.path.getmtime(fp)
                        fd = datetime.fromtimestamp(mtime, tz=timezone.utc)
                        if fd < cutoff:
                            os.remove(fp)
                            vd += 1
                    except OSError as e:
                        logger.warning(f"删除文件失败 {fp}: {e}")

            logger.info(f"清理完成: 删除{deleted}个传感器文件, {vd}个视频元数据文件")
        except Exception as e:
            logger.error(f"清理旧数据失败: {e}")

    def backup_data(self, backup_name: str = None) -> str:
        try:
            if backup_name is None:
                backup_name = datetime.now().strftime('backup_%Y%m%d_%H%M%S')
            backup_path = os.path.join(self.config.BACKUP_DIR, backup_name)
            os.makedirs(backup_path, exist_ok=True)

            sensor_bak = os.path.join(backup_path, 'sensor')
            if os.path.exists(self.config.SENSOR_DATA_DIR):
                shutil.copytree(self.config.SENSOR_DATA_DIR, sensor_bak,
                                dirs_exist_ok=True)

            video_bak = os.path.join(backup_path, 'video')
            if os.path.exists(self.config.VIDEO_DATA_DIR):
                shutil.copytree(self.config.VIDEO_DATA_DIR, video_bak,
                                dirs_exist_ok=True)

            logger.info(f"数据备份完成: {backup_path}")
            return backup_path
        except Exception as e:
            logger.error(f"数据备份失败: {e}")
            raise
