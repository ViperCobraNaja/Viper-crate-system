"""MongoDB 数据库存储"""

import logging
from datetime import datetime, timezone, timedelta
from typing import Dict, Any, List, Optional

from config import Config

logger = logging.getLogger(__name__)


class DatabaseStorage:
    """MongoDB 存储管理"""

    def __init__(self, mongo_client):
        self.client = mongo_client
        self.config = Config()
        self.db = self.client[self.config.DATABASE_NAME]

        self.sensor_data = self.db["sensor_data"]
        self.video_metadata = self.db["video_metadata"]
        self.daily_reports = self.db["daily_reports"]
        self.device_status = self.db["device_status"]
        self.motion_events = self.db["motion_events"]
        self.device_commands = self.db["device_commands"]
        self.device_heartbeats = self.db["device_heartbeats"]

        self._ensure_indexes()
        logger.info("数据库存储初始化完成")

    def _ensure_indexes(self):
        """创建常用查询索引"""
        try:
            self.sensor_data.create_index([("device_id", 1), ("timestamp", -1)])
            self.video_metadata.create_index([("device_id", 1), ("create_time", -1)])
            self.motion_events.create_index([("device_id", 1), ("timestamp", -1)])
            self.device_heartbeats.create_index([("device_id", 1), ("timestamp", -1)])
            self.device_commands.create_index([("device_id", 1), ("status", 1)])
        except Exception as e:
            logger.warning(f"创建索引失败（可能无权限）: {e}")

    # ---- 传感器数据 ----

    def save_sensor_data(self, data: Dict[str, Any]) -> str:
        try:
            data['_db_inserted_at'] = datetime.now(timezone.utc)
            data['_storage_type'] = 'mongodb'
            result = self.sensor_data.insert_one(data)
            doc_id = str(result.inserted_id)
            logger.info(f"传感器数据保存到数据库: {doc_id}")
            return doc_id
        except Exception as e:
            logger.error(f"保存传感器数据到数据库失败: {e}")
            raise

    # ---- 视频元数据 ----

    def save_video_metadata(self, metadata: Dict[str, Any]) -> str:
        try:
            metadata['_db_inserted_at'] = datetime.now(timezone.utc)
            metadata['_storage_type'] = 'mongodb'
            result = self.video_metadata.insert_one(metadata)
            doc_id = str(result.inserted_id)
            logger.info(f"视频元数据保存到数据库: {doc_id}")
            return doc_id
        except Exception as e:
            logger.error(f"保存视频元数据到数据库失败: {e}")
            raise

    # ---- 设备状态 ----

    def update_device_status(self, status_data: Dict[str, Any]) -> bool:
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
            return success
        except Exception as e:
            logger.error(f"更新设备状态失败: {e}")
            return False

    def get_device_status(self, device_id: str) -> Optional[Dict[str, Any]]:
        try:
            status = self.device_status.find_one({"device_id": device_id})
            if status:
                status['_id'] = str(status['_id'])
                return status
            return None
        except Exception as e:
            logger.error(f"获取设备状态失败: {e}")
            return None

    # ---- 运动事件 ----

    def save_motion_event(self, event: dict) -> str:
        try:
            event['_db_inserted_at'] = datetime.now(timezone.utc)
            result = self.motion_events.insert_one(event)
            return str(result.inserted_id)
        except Exception as e:
            logger.error(f"保存运动事件失败: {e}")
            raise

    def query_motion_events(self, device_id: str = None,
                            start_time: datetime = None,
                            end_time: datetime = None,
                            limit: int = 100) -> List[dict]:
        try:
            query = {}
            if device_id:
                query["device_id"] = device_id
            if start_time or end_time:
                query["timestamp"] = {}
                if start_time:
                    query["timestamp"]["$gte"] = start_time
                if end_time:
                    query["timestamp"]["$lte"] = end_time
            cursor = self.motion_events.find(query).sort("timestamp", -1).limit(limit)
            results = []
            for doc in cursor:
                doc['_id'] = str(doc['_id'])
                results.append(doc)
            return results
        except Exception as e:
            logger.error(f"查询运动事件失败: {e}")
            return []

    # ---- 设备命令 ----

    def enqueue_command(self, device_id: str, command: str,
                        params: dict = None) -> Optional[str]:
        try:
            pending_count = self.device_commands.count_documents({
                "device_id": device_id, "status": "pending"
            })
            if pending_count >= 20:
                logger.warning(f"设备 {device_id} 待执行命令过多: {pending_count}")
                return None

            doc = {
                "device_id": device_id,
                "command": command,
                "params": params or {},
                "status": "pending",
                "created_at": datetime.now(timezone.utc),
                "executed_at": None,
            }
            result = self.device_commands.insert_one(doc)
            cmd_id = str(result.inserted_id)
            logger.info(f"命令入队: {cmd_id} -> {device_id}/{command}")
            return cmd_id
        except Exception as e:
            logger.error(f"命令入队失败: {e}")
            return None

    def poll_commands(self, device_id: str) -> List[dict]:
        try:
            cursor = self.device_commands.find({
                "device_id": device_id, "status": "pending"
            }).sort("created_at", 1)
            results = []
            for doc in cursor:
                doc['_id'] = str(doc['_id'])
                results.append(doc)
            return results
        except Exception as e:
            logger.error(f"轮询命令失败: {e}")
            return []

    def mark_command_executed(self, command_id: str, success: bool):
        try:
            from bson.objectid import ObjectId
            self.device_commands.update_one(
                {"_id": ObjectId(command_id)},
                {"$set": {
                    "status": "executed" if success else "failed",
                    "executed_at": datetime.now(timezone.utc),
                }}
            )
        except Exception as e:
            logger.error(f"标记命令状态失败: {e}")

    # ---- 设备心跳 ----

    def save_heartbeat(self, heartbeat: dict) -> str:
        try:
            heartbeat['_db_inserted_at'] = datetime.now(timezone.utc)
            result = self.device_heartbeats.insert_one(heartbeat)
            self.device_status.update_one(
                {"device_id": heartbeat.get('device_id')},
                {"$set": {
                    "online": True,
                    "last_heartbeat": heartbeat.get('timestamp'),
                    "last_cpu_usage": heartbeat.get('cpu_usage'),
                    "last_memory_usage": heartbeat.get('memory_usage'),
                    "last_storage_usage": heartbeat.get('storage_usage'),
                    "last_temperature": heartbeat.get('temperature'),
                    "last_uptime_ms": heartbeat.get('uptime_ms'),
                }},
                upsert=True
            )
            return str(result.inserted_id)
        except Exception as e:
            logger.error(f"保存心跳失败: {e}")
            raise

    def get_offline_devices(self, threshold_seconds: int = 600) -> List[str]:
        try:
            cutoff = datetime.now(timezone.utc) - timedelta(seconds=threshold_seconds)
            cursor = self.device_status.find({
                "last_heartbeat": {"$lt": cutoff.isoformat()},
                "online": True,
            })
            offline_ids = []
            for doc in cursor:
                device_id = doc.get('device_id')
                self.device_status.update_one(
                    {"device_id": device_id},
                    {"$set": {"online": False}}
                )
                offline_ids.append(device_id)
            return offline_ids
        except Exception as e:
            logger.error(f"检测离线设备失败: {e}")
            return []

    # ---- 统计 ----

    def get_sensor_stats(self, device_id: str = None, days: int = 7) -> dict:
        try:
            end_time = datetime.now(timezone.utc)
            start_time = end_time - timedelta(days=days)
            query = {"timestamp": {"$gte": start_time, "$lte": end_time}}
            if device_id:
                query["device_id"] = device_id

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
                            "$cond": [{"$eq": ["$sensors.motion.detected", True]}, 1, 0]
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
                    "total_count": 0, "unique_devices_count": 0,
                    "unique_devices": [], "avg_temperature": None,
                    "avg_humidity": None, "activity_count": 0,
                    "period": {"start": start_time.isoformat(),
                               "end": end_time.isoformat(), "days": days}
                }
            return stats
        except Exception as e:
            logger.error(f"获取传感器统计失败: {e}")
            return {}
