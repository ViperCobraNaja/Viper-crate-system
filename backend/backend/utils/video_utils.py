"""视频处理工具 —— ffmpeg 封装，H.264→MP4 转换"""

import subprocess
import logging
import os

logger = logging.getLogger(__name__)

# 可配置的 ffmpeg 路径
FFMPEG_PATH = os.environ.get('FFMPEG_PATH', 'ffmpeg')


def h264_to_mp4(input_path: str, output_path: str, fps: int = 20) -> bool:
    """将裸 H.264 Annex B 码流封装为 MP4 容器

    使用 ffmpeg 的 stream copy 模式，不重新编码，仅更换容器格式。
    添加 faststart 标志使浏览器可以渐进式播放。

    Args:
        input_path: 输入 H.264 裸流文件路径
        output_path: 输出 MP4 文件路径
        fps: 帧率（用于设置 MP4 时间基）

    Returns:
        True 如果转换成功，False 如果失败
    """
    if not os.path.exists(input_path):
        logger.error(f"输入文件不存在: {input_path}")
        return False

    cmd = [
        FFMPEG_PATH, '-y',
        '-f', 'h264',
        '-r', str(fps),
        '-i', input_path,
        '-c', 'copy',
        '-movflags', 'faststart',
        output_path
    ]

    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=120)

        if result.returncode != 0:
            logger.error(f"ffmpeg 转换失败: {result.stderr[:500]}")
            return False

        if os.path.exists(output_path) and os.path.getsize(output_path) > 0:
            logger.info(f"MP4 转换成功: {output_path}")
            return True
        else:
            logger.error("MP4 输出文件为空或不存在")
            return False

    except FileNotFoundError:
        logger.error(
            "ffmpeg 未安装或路径不正确。"
            "macOS: brew install ffmpeg, "
            "Ubuntu: apt install ffmpeg, "
            "或设置 FFMPEG_PATH 环境变量"
        )
        return False
    except subprocess.TimeoutExpired:
        logger.error(f"ffmpeg 转换超时: {input_path}")
        return False
    except Exception as e:
        logger.error(f"ffmpeg 转换异常: {e}")
        return False


def get_video_info(filepath: str) -> dict:
    """使用 ffprobe 获取视频文件信息（用于调试）"""
    if not os.path.exists(filepath):
        return {}

    cmd = [
        'ffprobe', '-v', 'quiet', '-print_format', 'json',
        '-show_format', '-show_streams', filepath
    ]

    try:
        import json
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode == 0:
            return json.loads(result.stdout)
        return {}
    except Exception:
        return {}
