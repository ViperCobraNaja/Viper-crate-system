#!/usr/bin/env python3
"""
LLM服务测试脚本
"""

import sys
import os
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

from backend.llm_service import LLMService
from backend.config import Config

def test_llm_service():
    """测试LLM服务"""
    print("=== LLM服务测试 ===")

    # 创建配置
    config = Config()
    print(f"LLM服务启用状态: {config.LLM_ENABLED}")
    print(f"DeepSeek API Key 是否设置: {bool(config.DEEPSEEK_API_KEY)}")
    print(f"Qwen API Key 是否设置: {bool(config.QWEN_API_KEY)}")

    # 创建LLM服务实例
    try:
        llm_service = LLMService(config)
        print("✓ LLM服务实例创建成功")

        # 测试健康检查
        health = llm_service.health_check()
        print(f"✓ 健康检查: {health}")

        # 测试模型信息
        model_info = llm_service.get_model_info('deepseek_reasoner')
        print(f"✓ DeepSeek模型信息: {model_info}")

        model_info = llm_service.get_model_info('qwen_vl_plus')
        print(f"✓ Qwen-VL-Plus模型信息: {model_info}")

        # 测试缓存统计
        cache_stats = llm_service.get_cache_stats()
        print(f"✓ 缓存统计: {cache_stats}")

        print("\n=== LLM服务测试完成 ===")
        return True

    except Exception as e:
        print(f"✗ LLM服务测试失败: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == '__main__':
    test_llm_service()