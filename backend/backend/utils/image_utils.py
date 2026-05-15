"""
宠语者 - 图片处理工具
用于将图片数据转换为LLM API需要的格式
支持多种输入格式：numpy数组、bytes、文件路径、PIL Image
"""

import base64
import io
import logging
from typing import Union, Tuple, Optional
from pathlib import Path

import numpy as np
from PIL import Image

logger = logging.getLogger(__name__)


class ImageProcessingError(Exception):
    """图片处理错误"""
    pass


def image_to_base64(image_data: Union[np.ndarray, bytes, str, Image.Image],
                    format: str = "JPEG",
                    quality: int = 85) -> str:
    """
    将图片数据转换为base64编码字符串

    Args:
        image_data: 图片数据，支持以下格式：
            - numpy.ndarray: OpenCV格式的图片数组 (H, W, C) 或 (H, W)
            - bytes: 原始图片字节数据
            - str: 图片文件路径
            - PIL.Image: PIL图像对象
        format: 输出格式，如 "JPEG", "PNG"
        quality: JPEG质量 (1-100)

    Returns:
        base64编码的图片字符串

    Raises:
        ImageProcessingError: 图片处理失败时抛出
    """
    try:
        # 处理不同输入类型
        if isinstance(image_data, np.ndarray):
            return _numpy_to_base64(image_data, format, quality)
        elif isinstance(image_data, bytes):
            return _bytes_to_base64(image_data)
        elif isinstance(image_data, str):
            return _file_to_base64(image_data, format, quality)
        elif isinstance(image_data, Image.Image):
            return _pil_to_base64(image_data, format, quality)
        else:
            raise ImageProcessingError(f"不支持的图片数据类型: {type(image_data)}")

    except Exception as e:
        logger.error(f"图片转base64失败: {str(e)}")
        raise ImageProcessingError(f"图片处理失败: {str(e)}")


def _numpy_to_base64(image_array: np.ndarray, format: str = "JPEG", quality: int = 85) -> str:
    """将numpy数组转换为base64"""
    try:
        # 检查数组维度和数据类型
        if len(image_array.shape) not in [2, 3]:
            raise ImageProcessingError(f"无效的图像数组维度: {image_array.shape}")

        # 转换为PIL Image
        if len(image_array.shape) == 3:
            # RGB或BGR图像
            if image_array.shape[2] == 3:
                # 假设为RGB，如果是BGR需要转换
                pil_image = Image.fromarray(image_array.astype(np.uint8))
            elif image_array.shape[2] == 4:
                # RGBA图像
                pil_image = Image.fromarray(image_array.astype(np.uint8))
            else:
                raise ImageProcessingError(f"不支持的通道数: {image_array.shape[2]}")
        else:
            # 灰度图像
            pil_image = Image.fromarray(image_array.astype(np.uint8))

        return _pil_to_base64(pil_image, format, quality)

    except Exception as e:
        raise ImageProcessingError(f"numpy数组转换失败: {str(e)}")


def _bytes_to_base64(image_bytes: bytes) -> str:
    """将字节数据直接转换为base64"""
    try:
        # 直接编码为base64
        encoded = base64.b64encode(image_bytes).decode('utf-8')
        return encoded
    except Exception as e:
        raise ImageProcessingError(f"字节数据转换失败: {str(e)}")


def _file_to_base64(file_path: str, format: str = "JPEG", quality: int = 85) -> str:
    """从文件读取并转换为base64"""
    try:
        # 检查文件是否存在
        path = Path(file_path)
        if not path.exists():
            raise ImageProcessingError(f"图片文件不存在: {file_path}")

        # 使用PIL打开并转换
        with Image.open(file_path) as img:
            # 转换为RGB模式（如果必要）
            if img.mode not in ['RGB', 'RGBA', 'L']:
                img = img.convert('RGB')

            return _pil_to_base64(img, format, quality)

    except Exception as e:
        raise ImageProcessingError(f"文件处理失败: {str(e)}")


def _pil_to_base64(pil_image: Image.Image, format: str = "JPEG", quality: int = 85) -> str:
    """将PIL Image转换为base64"""
    try:
        # 确保图像为RGB模式
        if pil_image.mode not in ['RGB', 'RGBA', 'L']:
            pil_image = pil_image.convert('RGB')

        # 保存到内存缓冲区
        buffer = io.BytesIO()

        # 根据格式保存
        if format.upper() == 'PNG':
            pil_image.save(buffer, format='PNG')
        else:
            # 默认为JPEG
            pil_image.save(buffer, format='JPEG', quality=quality, optimize=True)

        # 获取字节并编码
        image_bytes = buffer.getvalue()
        encoded = base64.b64encode(image_bytes).decode('utf-8')

        return encoded

    except Exception as e:
        raise ImageProcessingError(f"PIL图像转换失败: {str(e)}")


def get_image_info(image_data: Union[np.ndarray, bytes, str, Image.Image]) -> dict:
    """
    获取图片信息

    Returns:
        包含图片宽度、高度、格式等信息的字典
    """
    try:
        if isinstance(image_data, np.ndarray):
            height, width = image_data.shape[:2]
            channels = 1 if len(image_data.shape) == 2 else image_data.shape[2]
            dtype = str(image_data.dtype)
            return {
                "width": width,
                "height": height,
                "channels": channels,
                "dtype": dtype,
                "format": "numpy_array"
            }

        elif isinstance(image_data, bytes):
            # 尝试从字节数据获取信息
            try:
                img = Image.open(io.BytesIO(image_data))
                return {
                    "width": img.width,
                    "height": img.height,
                    "mode": img.mode,
                    "format": img.format or "unknown",
                    "size_bytes": len(image_data)
                }
            except:
                return {
                    "size_bytes": len(image_data),
                    "format": "raw_bytes"
                }

        elif isinstance(image_data, str):
            # 文件路径
            with Image.open(image_data) as img:
                return {
                    "width": img.width,
                    "height": img.height,
                    "mode": img.mode,
                    "format": img.format or "unknown",
                    "file_path": image_data
                }

        elif isinstance(image_data, Image.Image):
            return {
                "width": image_data.width,
                "height": image_data.height,
                "mode": image_data.mode,
                "format": "PIL_Image"
            }

        else:
            return {"error": f"不支持的图片类型: {type(image_data)}"}

    except Exception as e:
        logger.error(f"获取图片信息失败: {str(e)}")
        return {"error": str(e)}


def prepare_image_for_qwen(image_data: Union[np.ndarray, bytes, str, Image.Image],
                          max_size: Optional[Tuple[int, int]] = None) -> dict:
    """
    准备图片数据用于Qwen-VL-Plus API

    Args:
        image_data: 图片数据
        max_size: 可选的最大尺寸 (width, height)，如果提供会等比例缩小

    Returns:
        Qwen-VL-Plus API需要的图片数据格式
    """
    try:
        # 获取base64编码
        base64_str = image_to_base64(image_data, format="JPEG", quality=85)

        # 获取图片信息
        info = get_image_info(image_data)

        # 构建Qwen-VL-Plus需要的格式
        # 参考：https://help.aliyun.com/zh/model-studio/developer-reference/multimodal-generation
        result = {
            "type": "image",
            "image": f"data:image/jpeg;base64,{base64_str}",
            "detail": "high"  # 高细节模式
        }

        # 添加尺寸信息（如果可用）
        if "width" in info and "height" in info:
            result["width"] = info["width"]
            result["height"] = info["height"]

        return result

    except Exception as e:
        logger.error(f"准备Qwen图片数据失败: {str(e)}")
        raise ImageProcessingError(f"准备Qwen图片数据失败: {str(e)}")


def prepare_image_for_deepseek(image_data: Union[np.ndarray, bytes, str, Image.Image]) -> str:
    """
    准备图片数据用于DeepSeek API（如果支持图片）

    Returns:
        base64编码的图片字符串
    """
    try:
        # DeepSeek的图片格式可能不同，暂时与Qwen相同
        base64_str = image_to_base64(image_data, format="JPEG", quality=85)

        # DeepSeek可能需要不同的格式
        # 参考：https://api-docs.deepseek.com/guides/vision
        return base64_str

    except Exception as e:
        logger.error(f"准备DeepSeek图片数据失败: {str(e)}")
        raise ImageProcessingError(f"准备DeepSeek图片数据失败: {str(e)}")


# 测试代码
if __name__ == '__main__':
    import sys

    # 配置日志
    logging.basicConfig(level=logging.INFO)

    print("测试图片处理工具...")

    # 测试1: 创建测试numpy数组
    print("\n1. 测试numpy数组:")
    try:
        test_array = np.random.randint(0, 255, (100, 100, 3), dtype=np.uint8)
        base64_result = image_to_base64(test_array)
        info = get_image_info(test_array)
        print(f"  成功转换numpy数组: {info}")
        print(f"  base64长度: {len(base64_result)}")
    except Exception as e:
        print(f"  测试失败: {e}")

    # 测试2: 模拟字节数据
    print("\n2. 测试字节数据:")
    try:
        # 创建一个简单的测试图片字节
        from PIL import Image as PILImage
        img = PILImage.new('RGB', (50, 50), color='red')
        buffer = io.BytesIO()
        img.save(buffer, format='JPEG')
        test_bytes = buffer.getvalue()

        base64_result = image_to_base64(test_bytes)
        info = get_image_info(test_bytes)
        print(f"  成功转换字节数据: {info}")
        print(f"  base64长度: {len(base64_result)}")
    except Exception as e:
        print(f"  测试失败: {e}")

    # 测试3: 测试Qwen格式准备
    print("\n3. 测试Qwen格式准备:")
    try:
        test_array = np.random.randint(0, 255, (80, 120, 3), dtype=np.uint8)
        qwen_format = prepare_image_for_qwen(test_array)
        print(f"  成功准备Qwen格式数据")
        print(f"  包含字段: {list(qwen_format.keys())}")
        if "image" in qwen_format:
            print(f"  图片数据前缀: {qwen_format['image'][:50]}...")
    except Exception as e:
        print(f"  测试失败: {e}")

    print("\n测试完成!")