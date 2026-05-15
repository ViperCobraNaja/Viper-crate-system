"""
宠语者 - 数据存储管理模块
负责传感器数据、视频数据和配置数据的本地存储和数据库管理
"""

import os
import json
import shutil
import logging
from datetime import datetime, timezone, timedelta
from typing import Dict, Any, List, Optional, Union
import hashlib

from config import Config

# 配置日志
logger = logging.getLogger(__name__)


class LocalStorage:
    """本地文件存储管理"""

    def __init__(self):
        self.config = Config()
        self._ensure_directories()
        logger.info("本地存储初始化完成")

    def _ensure_directories(self):
        """确保必要的目录存在"""
        directories = [
            self.config.DATA_DIR,
            self.config.SENSOR_DATA_DIR,
            self.config.VIDEO_DATA_DIR,
            self.config.BACKUP_DIR,
            self.config.LOG_DIR
        ]

        for directory in directories:
            os.makedirs(directory, exist_ok=True)
            logger.debug(f"确保目录存在: {directory}")

    def save_sensor_data(self, data: Dict[str, Any]) -> str:
        """
        保存传感器数据到本地文件

        Args:
            data: 传感器数据字典

        Returns:
            保存的文件路径
        """
        try:
            # 生成文件名
            device_id = data.get('device_id', 'unknown')
            timestamp = data.get('timestamp', datetime.now(timezone.utc).isoformat())

            # 解析时间戳
            if isinstance(timestamp, str):
                dt = datetime.fromisoformat(timestamp.replace('Z', '+00:00'))
            else:
                dt = timestamp

            # 创建文件名
            filename = f"{device_id}_{dt.strftime('%Y%m%d_%H%M%S')}.json"
            filepath = os.path.join(self.config.SENSOR_DATA_DIR, filename)

            # 添加存储时间
            data['_stored_at'] = datetime.now(timezone.utc).isoformat()
            data['_storage_type'] = 'local_file'

            # 保存文件
            with open(filepath, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2, ensure_ascii=False, default=str)

            logger.info(f"传感器数据保存到本地文件: {filepath}")
            return filepath

        except Exception as e:
            logger.error(f"保存传感器数据到本地文件失败: {str(e)}")
            raise

    def save_video_metadata(self, metadata: Dict[str, Any]) -> str:
        """
        保存视频元数据到本地文件

        Args:
            metadata: 视频元数据

        Returns:
            保存的文件路径
        """
        try:
            # 生成文件名
            video_id = metadata.get('video_id', 'unknown')
            timestamp = metadata.get('timestamp', datetime.now(timezone.utc).isoformat())

            if isinstance(timestamp, str):
                dt = datetime.fromisoformat(timestamp.replace('Z', '+00:00'))
            else:
                dt = timestamp

            filename = f"video_{video_id}_{dt.strftime('%Y%m%d_%H%M%S')}.json"
            filepath = os.path.join(self.config.VIDEO_DATA_DIR, filename)

            # 添加存储信息
            metadata['_stored_at'] = datetime.now(timezone.utc).isoformat()
            metadata['_storage_type'] = 'video_metadata'

            # 保存文件
            with open(filepath, 'w', encoding='utf-8') as f:
                json.dump(metadata, f, indent=2, ensure_ascii=False, default=str)

            logger.info(f"视频元数据保存到本地文件: {filepath}")
            return filepath

        except Exception as e:
            logger.error(f"保存视频元数据失败: {str(e)}")
            raise

    def get_sensor_history(self,
                          device_id: Optional[str] = None,
                          start_time: Optional[str] = None,
                          end_time: Optional[str] = None,
                          limit: int = 100) -> List[Dict[str, Any]]:
        """
        从本地文件获取传感器历史数据

        Args:
            device_id: 设备ID
            start_time: 开始时间（ISO格式）
            end_time: 结束时间（ISO格式）
            limit: 最大返回数量

        Returns:
            传感器数据列表
        """
        try:
            data_files = os.listdir(self.config.SENSOR_DATA_DIR)
            data_files = [f for f in data_files if f.endswith('.json')]

            # 按时间排序（最新的在前）
            data_files.sort(reverse=True)

            results = []
            for filename in data_files:
                if len(results) >= limit:
                    break

                filepath = os.path.join(self.config.SENSOR_DATA_DIR, filename)
                try:
                    with open(filepath, 'r', encoding='utf-8') as f:
                        data = json.load(f)

                    # 过滤条件
                    if device_id and data.get('device_id') != device_id:
                        continue

                    # 时间过滤
                    timestamp_str = data.get('timestamp')
                    if timestamp_str:
                        try:
                            data_time = datetime.fromisoformat(
                                timestamp_str.replace('Z', '+00:00')
                            )
                        except ValueError:
                            continue

                        if start_time:
                            start_dt = datetime.fromisoformat(start_time.replace('Z', '+00:00'))
                            if data_time < start_dt:
                                continue

                        if end_time:
                            end_dt = datetime.fromisoformat(end_time.replace('Z', '+00:00'))
                            if data_time > end_dt:
                                continue

                    results.append(data)

                except (json.JSONDecodeError, OSError) as e:
                    logger.warning(f"读取文件失败 {filepath}: {str(e)}")
                    continue

            logger.info(f"从本地文件加载{len(results)}条传感器数据")
            return results

        except Exception as e:
            logger.error(f"获取传感器历史数据失败: {str(e)}")
            return []

    def cleanup_old_data(self):
        """清理过期的本地数据"""
        try:
            cutoff_date = datetime.now(timezone.utc) - timedelta(
                days=self.config.SENSOR_DATA_RETENTION_DAYS
            )

            # 清理传感器数据
            sensor_files = os.listdir(self.config.SENSOR_DATA_DIR)
            deleted_sensor = 0
            for filename in sensor_files:
                if filename.endswith('.json'):
                    filepath = os.path.join(self.config.SENSOR_DATA_DIR, filename)
                    try:
                        # 从文件名提取时间
                        parts = filename.split('_')
                        if len(parts) >= 3:
                            date_str = parts[-2]
                            time_str = parts[-1].replace('.json', '')
                            try:
                                file_time = datetime.strptime(
                                    f"{date_str}_{time_str}", "%Y%m%d_%H%M%S"
                                )
                                if file_time < cutoff_date:
                                    os.remove(filepath)
                                    deleted_sensor += 1
                            except ValueError:
                                pass
                    except OSError as e:
                        logger.warning(f"删除文件失败 {filepath}: {str(e)}")

            # 清理视频元数据
            video_files = os.listdir(self.config.VIDEO_DATA_DIR)
            deleted_video = 0
            for filename in video_files:
                if filename.endswith('.json'):
                    filepath = os.path.join(self.config.VIDEO_DATA_DIR, filename)
                    try:
                        # 检查文件修改时间
                        mtime = os.path.getmtime(filepath)
                        file_date = datetime.fromtimestamp(mtime, tz=timezone.utc)
                        if file_date < cutoff_date:
                            os.remove(filepath)
                            deleted_video += 1
                    except OSError as e:
                        logger.warning(f"删除文件失败 {filepath}: {str(e)}")

            logger.info(f"清理完成: 删除{deleted_sensor}个传感器文件, {deleted_video}个视频文件")

        except Exception as e:
            logger.error(f"清理旧数据失败: {str(e)}")

    def backup_data(self, backup_name: str = None) -> str:
        """
        备份数据到备份目录

        Args:
            backup_name: 备份名称

        Returns:
            备份文件路径
        """
        try:
            if backup_name is None:
                backup_name = datetime.now().strftime('backup_%Y%m%d_%H%M%S')

            backup_path = os.path.join(self.config.BACKUP_DIR, backup_name)
            os.makedirs(backup_path, exist_ok=True)

            # 复制传感器数据
            sensor_backup = os.path.join(backup_path, 'sensor')
            if os.path.exists(self.config.SENSOR_DATA_DIR):
                shutil.copytree(self.config.SENSOR_DATA_DIR, sensor_backup, dirs_exist_ok=True)

            # 复制视频元数据
            video_backup = os.path.join(backup_path, 'video')
            if os.path.exists(self.config.VIDEO_DATA_DIR):
                shutil.copytree(self.config.VIDEO_DATA_DIR, video_backup, dirs_exist_ok=True)

            logger.info(f"数据备份完成: {backup_path}")
            return backup_path

        except Exception as e:
            logger.error(f"数据备份失败: {str(e)}")
            raise


class DatabaseStorage:
    """数据库存储管理"""

    def __init__(self, mongo_client):
        self.client = mongo_client
        self.config = Config()
        self.db = self.client[self.config.DATABASE_NAME]

        # 获取集合
        self.sensor_data = self.db["sensor_data"]
        self.video_metadata = self.db["video_metadata"]
        self.daily_reports = self.db["daily_reports"]
        self.device_status = self.db["device_status"]

        logger.info("数据库存储初始化完成")

    def save_sensor_data(self, data: Dict[str, Any]) -> str:
        """
        保存传感器数据到数据库

        Args:
            data: 传感器数据

        Returns:
            文档ID
        """
        try:
            # 添加数据库时间戳
            data['_db_inserted_at'] = datetime.now(timezone.utc)
            data['_storage_type'] = 'mongodb'

            # 插入数据
            result = self.sensor_data.insert_one(data)
            doc_id = str(result.inserted_id)

            logger.info(f"传感器数据保存到数据库: {doc_id}")
            return doc_id

        except Exception as e:
            logger.error(f"保存传感器数据到数据库失败: {str(e)}")
            raise

    def save_video_metadata(self, metadata: Dict[str, Any]) -> str:
        """
        保存视频元数据到数据库

        Args:
            metadata: 视频元数据

        Returns:
            文档ID
        """
        try:
            metadata['_db_inserted_at'] = datetime.now(timezone.utc)
            metadata['_storage_type'] = 'mongodb'

            result = self.video_metadata.insert_one(metadata)
            doc_id = str(result.inserted_id)

            logger.info(f"视频元数据保存到数据库: {doc_id}")
            return doc_id

        except Exception as e:
            logger.error(f"保存视频元数据到数据库失败: {str(e)}")
            raise

    def update_device_status(self, status_data: Dict[str, Any]) -> bool:
        """
        更新设备状态

        Args:
            status_data: 设备状态数据

        Returns:
            是否成功
        """
        try:
            device_id = status_data.get('device_id')
            if not device_id:
                raise ValueError("缺少device_id")

            status_data['_last_updated'] = datetime.now(timezone.utc)

            result = self.device_status.update_one(
                {"device_id": device_id},
                {"$set": status_data},
                upsert=True
            )

            success = result.modified_count > 0 or result.upserted_id is not None
            if success:
                logger.info(f"设备状态更新成功: {device_id}")
            else:
                logger.warning(f"设备状态更新失败: {device_id}")

            return success

        except Exception as e:
            logger.error(f"更新设备状态失败: {str(e)}")
            return False

    def get_device_status(self, device_id: str) -> Optional[Dict[str, Any]]:
        """获取设备状态"""
        try:
            status = self.device_status.find_one({"device_id": device_id})
            if status:
                status['_id'] = str(status['_id'])
                return status
            return None

        except Exception as e:
            logger.error(f"获取设备状态失败: {str(e)}")
            return None

    def get_sensor_stats(self, device_id: str = None,
                        days: int = 7) -> Dict[str, Any]:
        """
        获取传感器数据统计

        Args:
            device_id: 设备ID
            days: 统计天数

        Returns:
            统计数据
        """
        try:
            # 计算时间范围
            end_time = datetime.now(timezone.utc)
            start_time = end_time - timedelta(days=days)

            # 构建查询条件
            query = {
                "timestamp": {
                    "$gte": start_time,
                    "$lte": end_time
                }
            }
            if device_id:
                query["device_id"] = device_id

            # 执行聚合查询
            pipeline = [
                {"$match": query},
                {"$group": {
                    "_id": None,
                    "total_count": {"$sum": 1},
                    "unique_devices": {"$addToSet": "$device_id"},
                    "avg_temperature": {"$avg": "$sensors.temperature.value"},
                    "avg_humidity": {"$avg": "$sensors.humidity.value"},
                    "activity_count": {
                        "$sum": {
                            "$cond": [
                                {"$eq": ["$sensors.motion.detected", True]},
                                1,
                                0
                            ]
                        }
                    }
                }}
            ]

            result = list(self.sensor_data.aggregate(pipeline))
            if result:
                stats = result[0]
                stats.pop('_id', None)
                stats['unique_devices_count'] = len(stats.get('unique_devices', []))
                stats['unique_devices'] = list(stats.get('unique_devices', []))
                stats['period'] = {
                    "start": start_time.isoformat(),
                    "end": end_time.isoformat(),
                    "days": days
                }
            else:
                stats = {
                    "total_count": 0,
                    "unique_devices_count": 0,
                    "unique_devices": [],
                    "avg_temperature": None,
                    "avg_humidity": None,
                    "activity_count": 0,
                    "period": {
                        "start": start_time.isoformat(),
                        "end": end_time.isoformat(),
                        "days": days
                    }
                }

            logger.info(f"获取传感器统计: {stats['total_count']}条数据")
            return stats

        except Exception as e:
            logger.error(f"获取传感器统计失败: {str(e)}")
            return {}


class DataStorageManager:
    """数据存储管理器（集成本地和数据库存储）"""

    def __init__(self, mongo_client=None):
        self.config = Config()

        # 初始化本地存储
        self.local_storage = LocalStorage()

        # 初始化数据库存储（如果可用）
        self.db_storage = None
        if mongo_client:
            try:
                self.db_storage = DatabaseStorage(mongo_client)
                logger.info("数据存储管理器初始化完成（带数据库）")
            except Exception as e:
                logger.warning(f"数据库存储初始化失败，仅使用本地存储: {str(e)}")
                self.db_storage = None
        else:
            logger.info("数据存储管理器初始化完成（仅本地存储）")

    def save_sensor_data(self, data: Dict[str, Any]) -> Dict[str, Any]:
        """
        保存传感器数据（同时保存到本地和数据库）

        Args:
            data: 传感器数据

        Returns:
            存储结果
        """
        result = {
            "local": None,
            "database": None,
            "success": False
        }

        try:
            # 保存到本地
            local_path = self.local_storage.save_sensor_data(data)
            result["local"] = local_path

            # 保存到数据库（如果可用）
            if self.db_storage:
                db_id = self.db_storage.save_sensor_data(data)
                result["database"] = db_id

            result["success"] = True
            logger.info(f"传感器数据保存完成: 本地={local_path}, 数据库={result.get('database')}")

        except Exception as e:
            logger.error(f"保存传感器数据失败: {str(e)}")
            result["error"] = str(e)

        return result

    def save_video_metadata(self, metadata: Dict[str, Any]) -> Dict[str, Any]:
        """
        保存视频元数据

        Args:
            metadata: 视频元数据

        Returns:
            存储结果
        """
        result = {
            "local": None,
            "database": None,
            "success": False
        }

        try:
            # 保存到本地
            local_path = self.local_storage.save_video_metadata(metadata)
            result["local"] = local_path

            # 保存到数据库（如果可用）
            if self.db_storage:
                db_id = self.db_storage.save_video_metadata(metadata)
                result["database"] = db_id

            result["success"] = True
            logger.info(f"视频元数据保存完成: 本地={local_path}, 数据库={result.get('database')}")

        except Exception as e:
            logger.error(f"保存视频元数据失败: {str(e)}")
            result["error"] = str(e)

        return result

    def update_device_status(self, status_data: Dict[str, Any]) -> bool:
        """更新设备状态"""
        try:
            if self.db_storage:
                return self.db_storage.update_device_status(status_data)
            return False
        except Exception as e:
            logger.error(f"更新设备状态失败: {str(e)}")
            return False

    def get_sensor_history(self, device_id: str = None,
                          start_time: str = None,
                          end_time: str = None,
                          limit: int = 100) -> List[Dict[str, Any]]:
        """
        获取传感器历史数据（优先从数据库获取）

        Args:
            device_id: 设备ID
            start_time: 开始时间
            end_time: 结束时间
            limit: 最大数量

        Returns:
            传感器数据列表
        """
        try:
            if self.db_storage:
                # TODO: 实现数据库查询
                pass

            # 回退到本地文件查询
            return self.local_storage.get_sensor_history(
                device_id, start_time, end_time, limit
            )
        except Exception as e:
            logger.error(f"获取传感器历史数据失败: {str(e)}")
            return []

    def cleanup(self):
        """清理旧数据"""
        try:
            self.local_storage.cleanup_old_data()
            logger.info("数据清理完成")
        except Exception as e:
            logger.error(f"数据清理失败: {str(e)}")

    def backup(self, backup_name: str = None) -> str:
        """创建数据备份"""
        try:
            return self.local_storage.backup_data(backup_name)
        except Exception as e:
            logger.error(f"数据备份失败: {str(e)}")
            raise


# 测试函数
if __name__ == '__main__':
    # 配置日志
    logging.basicConfig(level=logging.INFO)

    # 创建存储管理器
    storage_manager = DataStorageManager()

    # 创建测试数据
    test_data = {
        "device_id": "ESP32-P4-001",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "sensors": {
            "temperature": {"value": 25.5, "unit": "celsius"},
            "humidity": {"value": 60, "unit": "percent"}
        }
    }

    # 测试保存
    print("测试保存传感器数据...")
    result = storage_manager.save_sensor_data(test_data)
    print(f"保存结果: {json.dumps(result, indent=2, ensure_ascii=False)}")

    # 测试查询
    print("\n测试查询历史数据...")
    history = storage_manager.get_sensor_history(limit=5)
    print(f"查询到{len(history)}条数据")

    # 测试备份
    print("\n测试数据备份...")
    try:
        backup_path = storage_manager.backup()
        print(f"备份路径: {backup_path}")
    except Exception as e:
        print(f"备份失败: {e}")

    print("\n测试完成")