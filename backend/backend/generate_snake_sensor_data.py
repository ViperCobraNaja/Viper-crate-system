#!/usr/bin/env python3
"""
宠语者 - 宠物蛇传感器数据生成器
用于生成符合宠物蛇特征的模拟传感器数据
"""

import json
import random
import time
from datetime import datetime, timezone
from typing import Dict, Any, List
import argparse


def generate_snake_sensor_data(device_id: str = "ESP32-P4-Snake-001", timestamp: datetime = None) -> Dict[str, Any]:
    """
    生成单条宠物蛇传感器数据

    Args:
        device_id: 设备ID
        timestamp: 时间戳，如果为None则使用当前时间

    Returns:
        传感器数据字典
    """
    if timestamp is None:
        timestamp = datetime.now(timezone.utc)

    # 宠物蛇的典型环境参数
    # 温度: 蛇类适宜温度通常在26-32°C之间
    temperature = round(random.uniform(26.0, 32.0), 1)

    # 湿度: 蛇类适宜湿度通常在50-70%之间
    humidity = round(random.uniform(50.0, 70.0), 1)

    # 光照: 蛇类对光照要求不高，一般在50-300 lux之间
    light = random.randint(50, 300)

    # 运动检测: 蛇类活动相对较少，检测概率较低
    motion_detected = random.random() < 0.3  # 30%概率检测到运动
    motion_confidence = round(random.uniform(0.6, 0.95), 2) if motion_detected else 0.0

    # 位置信息（蛇类通常在特定区域活动）
    motion_location = {
        "x": random.randint(0, 640),
        "y": random.randint(0, 480)
    } if motion_detected else None

    # 宠物蛇的特殊行为数据
    snake_behavior = {
        "body_temperature": round(random.uniform(28.0, 30.0), 1),  # 蛇体温
        "coiling_behavior": random.choice(["normal", "coiled", "resting"]),  # 蛇的行为状态
        "feeding_indication": random.choice(["none", "feeding", "digesting"])  # 饲喂指示
    }

    # 构建传感器数据
    sensor_data = {
        "device_id": device_id,
        "timestamp": timestamp.isoformat(),
        "sensors": {
            "temperature": {
                "value": temperature,
                "unit": "celsius"
            },
            "humidity": {
                "value": humidity,
                "unit": "percent"
            },
            "light": {
                "value": light,
                "unit": "lux"
            },
            "motion": {
                "detected": motion_detected,
                "confidence": motion_confidence,
                "location": motion_location
            }
        },
        "snake_behavior": snake_behavior,
        "status": {
            "battery": random.randint(70, 100),
            "storage_available": random.randint(2048, 8192),
            "wifi_connected": True
        }
    }

    return sensor_data


def generate_batch_sensor_data(count: int = 10, device_id: str = "ESP32-P4-Snake-001") -> List[Dict[str, Any]]:
    """
    生成批量传感器数据

    Args:
        count: 生成数据的数量
        device_id: 设备ID

    Returns:
        传感器数据列表
    """
    batch_data = []
    base_time = datetime.now(timezone.utc)

    for i in range(count):
        # 时间递增，模拟连续数据
        timestamp = base_time.replace(microsecond=0) - timedelta(minutes=i)
        data = generate_snake_sensor_data(device_id, timestamp)
        batch_data.append(data)

    return batch_data


def save_to_file(data: Dict[str, Any], filename: str):
    """
    将数据保存到文件

    Args:
        data: 要保存的数据
        filename: 文件名
    """
    with open(filename, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    print(f"数据已保存到 {filename}")


def main():
    """主函数"""
    parser = argparse.ArgumentParser(description='生成宠物蛇传感器数据')
    parser.add_argument('--count', type=int, default=1, help='生成数据条数 (默认: 1)')
    parser.add_argument('--device-id', type=str, default='ESP32-P4-Snake-001', help='设备ID (默认: ESP32-P4-Snake-001)')
    parser.add_argument('--batch', action='store_true', help='批量生成模式')
    parser.add_argument('--output', type=str, help='输出文件名')

    args = parser.parse_args()

    if args.batch:
        # 批量生成
        data_list = generate_batch_sensor_data(args.count, args.device_id)

        if args.output:
            # 保存到文件
            save_to_file(data_list, args.output)

            # 同时打印第一条数据
            print("生成的传感器数据:")
            print(json.dumps(data_list[0], indent=2, ensure_ascii=False))
        else:
            # 打印所有数据
            print("生成的传感器数据:")
            for i, data in enumerate(data_list):
                print(f"--- 数据 {i+1} ---")
                print(json.dumps(data, indent=2, ensure_ascii=False))
                print()

    else:
        # 单条生成
        data = generate_snake_sensor_data(args.device_id)

        if args.output:
            # 保存到文件
            save_to_file(data, args.output)
        else:
            # 打印数据
            print("生成的传感器数据:")
            print(json.dumps(data, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    from datetime import timedelta
    main()