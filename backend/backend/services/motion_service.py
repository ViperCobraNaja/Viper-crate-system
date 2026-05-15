"""运动检测事件服务"""

import logging
from datetime import datetime, timezone, timedelta
from typing import List, Optional

from models.motion import MotionEvent, MotionHeatmap, MOTION_GRID_SIZE
from storage import DataStorageManager

logger = logging.getLogger(__name__)


class MotionService:
    """运动事件业务逻辑：接收、存储、分析运动检测数据"""

    def __init__(self, storage: DataStorageManager):
        self.storage = storage
        logger.info("运动事件服务初始化完成")

    def ingest_event(self, data: dict) -> MotionEvent:
        """接收并存储运动事件"""
        event = MotionEvent.from_dict(data)
        self.storage.save_motion_event(event.to_dict())
        logger.info(f"运动事件已记录: {event.device_id} level={event.motion_level}")
        return event

    def query_events(self, device_id: str = None,
                     start_time: str = None, end_time: str = None,
                     limit: int = 100) -> List[dict]:
        """查询运动事件历史"""
        st = None
        et = None
        if start_time:
            st = datetime.fromisoformat(start_time.replace('Z', '+00:00'))
        if end_time:
            et = datetime.fromisoformat(end_time.replace('Z', '+00:00'))
        return self.storage.query_motion_events(device_id, st, et, limit)

    def get_heatmap_trend(self, device_id: str,
                          hours: int = 24) -> dict:
        """聚合一段时间内的运动热度图趋势

        将所有事件的热度图叠加求平均，返回整体活动热力分布
        """
        now = datetime.now(timezone.utc)
        start = now - timedelta(hours=hours)
        events = self.storage.query_motion_events(device_id, start, now, limit=10000)

        if not events:
            return {'hours': hours, 'event_count': 0, 'trend': None}

        # 叠加所有事件的热度图
        accumulated = [0] * MOTION_GRID_SIZE
        event_count = 0
        total_motion = 0

        for evt in events:
            heatmap = evt.get('heatmap', {})
            grid = heatmap.get('grid_motion', [0] * MOTION_GRID_SIZE)
            for i in range(min(len(grid), MOTION_GRID_SIZE)):
                accumulated[i] += grid[i]
            total_motion += heatmap.get('total_motion', 0)
            event_count += 1

        avg_grid = [round(v / event_count, 2) for v in accumulated] if event_count else accumulated

        # 按时间分桶（每小时）
        hourly_activity = self._bucket_hourly_activity(events, hours)

        return {
            'hours': hours,
            'event_count': event_count,
            'total_motion': total_motion,
            'avg_motion_per_event': round(total_motion / event_count, 2) if event_count else 0,
            'avg_heatmap_grid': avg_grid,
            'hourly_activity': hourly_activity,
        }

    def _bucket_hourly_activity(self, events: List[dict],
                                hours: int) -> List[dict]:
        now = datetime.now(timezone.utc)
        buckets = {}
        for i in range(hours):
            key = (now - timedelta(hours=hours - i)).strftime('%H:00')
            buckets[key] = {'count': 0, 'total_motion': 0}

        for evt in events:
            ts_str = evt.get('timestamp', '')
            if ts_str:
                try:
                    ts = datetime.fromisoformat(ts_str.replace('Z', '+00:00'))
                    key = ts.strftime('%H:00')
                    if key in buckets:
                        buckets[key]['count'] += 1
                        buckets[key]['total_motion'] += evt.get(
                            'heatmap', {}).get('total_motion', 0)
                except (ValueError, KeyError):
                    pass

        return [{'hour': k, **v} for k, v in buckets.items()]
