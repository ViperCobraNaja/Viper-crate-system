"""
宠语者 - LLM服务核心
提供统一的LLM API调用接口，支持DeepSeek和Qwen-VL-Plus
包含重试、缓存、错误处理等高级功能
"""

import json
import time
import logging
import hashlib
import base64
from typing import Dict, Any, Optional, Union, List
from urllib.parse import urlparse
import threading
from datetime import datetime, timedelta
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

from config import Config
from utils.image_utils import image_to_base64, prepare_image_for_qwen, ImageProcessingError

logger = logging.getLogger(__name__)


class LLMError(Exception):
    """LLM服务错误基类"""
    pass


class LLMConnectionError(LLMError):
    """LLM连接错误"""
    pass


class LLMResponseError(LLMError):
    """LLM响应错误"""
    pass


class LLMTimeoutError(LLMError):
    """LLM超时错误"""
    pass


class LLMService:
    """LLM服务核心类"""

    def __init__(self, config: Config):
        """
        初始化LLM服务

        Args:
            config: 配置对象
        """
        self.config = config
        self._loaded_config = None
        self._api_keys = {}
        self._session = None
        self._cache = {}  # 简单内存缓存
        self._cache_lock = threading.Lock()  # 缓存线程锁

        # 初始化
        self._initialize_session()
        self._load_config()
        self._load_api_keys()

    def _initialize_session(self):
        """初始化HTTP会话和重试策略"""
        self._session = requests.Session()

        # 配置重试策略
        retry_strategy = Retry(
            total=self.config.LLM_MAX_RETRIES,
            backoff_factor=self.config.LLM_BACKOFF_FACTOR,
            status_forcelist=[429, 500, 502, 503, 504],
            allowed_methods=["HEAD", "GET", "OPTIONS", "POST"]
        )

        adapter = HTTPAdapter(max_retries=retry_strategy)
        self._session.mount("http://", adapter)
        self._session.mount("https://", adapter)

    def _load_config(self):
        """加载LLM配置文件"""
        try:
            with open(self.config.LLM_CONFIG_PATH, 'r', encoding='utf-8') as f:
                self._loaded_config = json.load(f)
            logger.info("LLM配置文件加载成功")
        except Exception as e:
            logger.error(f"加载LLM配置文件失败: {e}")
            # 使用默认配置
            self._loaded_config = {
                "models": {},
                "tasks": {}
            }

    def _load_api_keys(self):
        """从配置和环境变量加载API密钥"""
        # 从环境变量加载
        if self.config.DEEPSEEK_API_KEY:
            self._api_keys['deepseek'] = self.config.DEEPSEEK_API_KEY
        else:
            logger.warning("未设置DEEPSEEK_API_KEY")

        if self.config.QWEN_API_KEY:
            self._api_keys['qwen'] = self.config.QWEN_API_KEY
        else:
            logger.warning("未设置QWEN_API_KEY")

    def _get_model_config(self, model_name: str) -> Optional[Dict[str, Any]]:
        """获取模型配置"""
        if not self._loaded_config or 'models' not in self._loaded_config:
            return None

        return self._loaded_config['models'].get(model_name)

    def _get_task_config(self, task_name: str) -> Optional[Dict[str, Any]]:
        """获取任务配置"""
        if not self._loaded_config or 'tasks' not in self._loaded_config:
            return None

        return self._loaded_config['tasks'].get(task_name)

    def _get_api_key(self, model_name: str) -> str:
        """获取指定模型的API密钥"""
        model_config = self._get_model_config(model_name)
        if not model_config:
            raise LLMError(f"未找到模型配置: {model_name}")

        # 从配置中获取环境变量名
        env_var = model_config.get('api_key_env_var')
        if env_var:
            return self._api_keys.get(env_var.lower().replace('_', ''), '')

        # 如果没有环境变量名，直接返回配置中的密钥
        return model_config.get('api_key', '')

    def _prepare_headers(self, model_name: str, is_multimodal: bool = False) -> Dict[str, str]:
        """准备API请求头"""
        headers = {
            'Content-Type': 'application/json',
            'User-Agent': 'PetBox-LLM-Service/1.0'
        }

        # 根据模型类型设置不同的头部
        model_config = self._get_model_config(model_name)
        if not model_config:
            raise LLMError(f"未找到模型配置: {model_name}")

        provider = model_config.get('provider', '').lower()

        if provider == 'deepseek':
            api_key = self._get_api_key(model_name)
            if not api_key:
                raise LLMError(f"未设置DeepSeek API密钥")
            headers['Authorization'] = f"Bearer {api_key}"

        elif provider == 'qwen':
            api_key = self._get_api_key(model_name)
            if not api_key:
                raise LLMError(f"未设置Qwen API密钥")
            headers['Authorization'] = f"Bearer {api_key}"
            headers['X-DashScope-Api-Key'] = api_key

        return headers

    def _should_use_cache(self, task_name: str) -> bool:
        """判断是否应该使用缓存"""
        if not self.config.LLM_CACHE_ENABLED:
            return False

        task_config = self._get_task_config(task_name)
        if not task_config:
            return False

        # 检查任务是否支持缓存
        return task_config.get('cache_enabled', True)

    def _generate_cache_key(self, task_name: str, input_data: Dict[str, Any]) -> str:
        """生成缓存键"""
        # 创建一个包含任务名和输入数据的哈希值
        cache_input = {
            'task': task_name,
            'input': input_data,
            'timestamp': datetime.utcnow().isoformat()
        }

        cache_str = json.dumps(cache_input, sort_keys=True, ensure_ascii=False)
        return hashlib.md5(cache_str.encode('utf-8')).hexdigest()

    def _get_cached_result(self, cache_key: str) -> Optional[Any]:
        """从缓存获取结果"""
        if not self.config.LLM_CACHE_ENABLED:
            return None

        with self._cache_lock:
            if cache_key in self._cache:
                cached_item = self._cache[cache_key]
                # 检查是否过期
                if datetime.utcnow() < cached_item['expires_at']:
                    logger.debug(f"缓存命中: {cache_key}")
                    return cached_item['result']
                else:
                    # 缓存过期，删除
                    del self._cache[cache_key]

        return None

    def _store_cached_result(self, cache_key: str, result: Any):
        """存储结果到缓存"""
        if not self.config.LLM_CACHE_ENABLED:
            return

        expires_at = datetime.utcnow() + timedelta(seconds=self.config.LLM_CACHE_TTL)

        with self._cache_lock:
            self._cache[cache_key] = {
                'result': result,
                'expires_at': expires_at
            }

    def _retry_request(self, request_func, max_retries: int = None, delay: float = None) -> Any:
        """带重试的请求执行"""
        if max_retries is None:
            max_retries = self.config.LLM_MAX_RETRIES
        if delay is None:
            delay = self.config.LLM_RETRY_DELAY

        last_exception = None

        for attempt in range(max_retries + 1):
            try:
                return request_func()
            except requests.exceptions.Timeout as e:
                last_exception = LLMTimeoutError(f"请求超时 (尝试 {attempt + 1}/{max_retries + 1}): {str(e)}")
                if attempt < max_retries:
                    logger.warning(f"请求超时，等待 {delay} 秒后重试...")
                    time.sleep(delay)
                    delay *= self.config.LLM_BACKOFF_FACTOR
                else:
                    raise last_exception
            except requests.exceptions.ConnectionError as e:
                last_exception = LLMConnectionError(f"连接错误 (尝试 {attempt + 1}/{max_retries + 1}): {str(e)}")
                if attempt < max_retries:
                    logger.warning(f"连接错误，等待 {delay} 秒后重试...")
                    time.sleep(delay)
                    delay *= self.config.LLM_BACKOFF_FACTOR
                else:
                    raise last_exception
            except Exception as e:
                # 其他错误直接抛出
                raise LLMError(f"请求失败 (尝试 {attempt + 1}/{max_retries + 1}): {str(e)}")

        if last_exception:
            raise last_exception

    def call_model(self, task_name: str, input_data: Dict[str, Any],
                   model_name: Optional[str] = None, **kwargs) -> Dict[str, Any]:
        """
        调用LLM模型

        Args:
            task_name: 任务名称
            input_data: 输入数据
            model_name: 指定模型名称（可选）
            **kwargs: 其他参数

        Returns:
            API响应结果

        Raises:
            LLMError: LLM相关错误
        """
        if not self.config.LLM_ENABLED:
            raise LLMError("LLM服务已被禁用")

        # 获取任务配置
        task_config = self._get_task_config(task_name)
        if not task_config:
            raise LLMError(f"未找到任务配置: {task_name}")

        # 确定使用的模型
        if model_name is None:
            model_name = task_config.get('primary_model')

        if not model_name:
            raise LLMError(f"任务 {task_name} 没有指定主模型")

        # 检查是否应该使用缓存
        cache_key = None
        if self._should_use_cache(task_name):
            cache_key = self._generate_cache_key(task_name, input_data)
            cached_result = self._get_cached_result(cache_key)
            if cached_result:
                return cached_result

        # 准备请求
        try:
            logger.debug(f"调用模型 {model_name} 执行任务 {task_name}")

            # 根据模型类型执行不同的调用
            model_config = self._get_model_config(model_name)
            if not model_config:
                raise LLMError(f"未找到模型配置: {model_name}")

            provider = model_config.get('provider', '').lower()

            if provider == 'deepseek':
                result = self._call_deepseek(task_name, input_data, model_config, **kwargs)
            elif provider == 'qwen':
                result = self._call_qwen(task_name, input_data, model_config, **kwargs)
            else:
                raise LLMError(f"不支持的模型提供商: {provider}")

            # 存储到缓存
            if cache_key:
                self._store_cached_result(cache_key, result)

            return result

        except Exception as e:
            logger.error(f"调用模型失败: {task_name}, 错误: {str(e)}")
            # 如果启用了降级，尝试备用模型
            if self.config.LLM_FALLBACK_ENABLED and 'fallback_model' in task_config:
                fallback_model = task_config['fallback_model']
                logger.info(f"尝试备用模型: {fallback_model}")
                try:
                    return self.call_model(task_name, input_data, fallback_model, **kwargs)
                except Exception as fallback_e:
                    logger.error(f"备用模型也失败: {str(fallback_e)}")

            # 如果没有降级选项或者降级也失败，重新抛出原始错误
            raise

    def _call_deepseek(self, task_name: str, input_data: Dict[str, Any],
                      model_config: Dict[str, Any], **kwargs) -> Dict[str, Any]:
        """调用DeepSeek API"""
        # 构建请求参数
        api_endpoint = model_config.get('api_endpoint', '')
        if not api_endpoint:
            raise LLMError("未配置DeepSeek API端点")

        headers = self._prepare_headers('deepseek_reasoner')

        # 构建消息内容
        messages = self._build_deepseek_messages(task_name, input_data, model_config)

        # 构建请求体
        payload = {
            'model': model_config.get('name', 'deepseek-reasoner'),
            'messages': messages,
            'temperature': model_config.get('temperature', 0.3),
            'max_tokens': model_config.get('max_tokens', 4096),
            'top_p': model_config.get('top_p', 0.9),
            'frequency_penalty': model_config.get('frequency_penalty', 0.0),
            'presence_penalty': model_config.get('presence_penalty', 0.0)
        }

        # 添加额外参数
        payload.update(kwargs)

        # 发送请求
        def request_func():
            response = self._session.post(
                api_endpoint,
                headers=headers,
                json=payload,
                timeout=self.config.LLM_TIMEOUT
            )
            response.raise_for_status()
            return response.json()

        try:
            result = self._retry_request(request_func)
            return self._parse_deepseek_response(result)
        except Exception as e:
            raise LLMError(f"DeepSeek API调用失败: {str(e)}")

    def _build_deepseek_messages(self, task_name: str, input_data: Dict[str, Any],
                                model_config: Dict[str, Any]) -> List[Dict[str, Any]]:
        """构建DeepSeek消息格式"""
        task_config = self._get_task_config(task_name)
        if not task_config:
            raise LLMError(f"未找到任务配置: {task_name}")

        # 获取系统提示
        system_prompt = self._get_system_prompt('deepseek_reasoner')

        # 获取任务提示模板
        prompt_templates = task_config.get('prompt_templates', {})
        default_prompt = prompt_templates.get('default', '')

        # 格式化提示
        formatted_prompt = self._format_prompt(default_prompt, input_data)

        messages = [
            {
                'role': 'system',
                'content': system_prompt
            },
            {
                'role': 'user',
                'content': formatted_prompt
            }
        ]

        return messages

    def _parse_deepseek_response(self, response: Dict[str, Any]) -> Dict[str, Any]:
        """解析DeepSeek API响应"""
        try:
            # DeepSeek返回的是聊天补全格式
            choices = response.get('choices', [])
            if not choices:
                raise LLMResponseError("API响应为空")

            message = choices[0].get('message', {})
            content = message.get('content', '')

            # 尝试解析JSON
            try:
                parsed_content = json.loads(content.strip())
                return {
                    'success': True,
                    'model': response.get('model', ''),
                    'usage': response.get('usage', {}),
                    'result': parsed_content,
                    'raw_response': response
                }
            except json.JSONDecodeError:
                # 如果不是JSON，返回纯文本
                return {
                    'success': True,
                    'model': response.get('model', ''),
                    'usage': response.get('usage', {}),
                    'result': content,
                    'raw_response': response
                }

        except Exception as e:
            raise LLMResponseError(f"解析DeepSeek响应失败: {str(e)}")

    def _call_qwen(self, task_name: str, input_data: Dict[str, Any],
                  model_config: Dict[str, Any], **kwargs) -> Dict[str, Any]:
        """调用Qwen API"""
        # 构建请求参数
        api_endpoint = model_config.get('api_endpoint', '')
        if not api_endpoint:
            raise LLMError("未配置Qwen API端点")

        headers = self._prepare_headers('qwen_vl_plus', is_multimodal=True)

        # 构建请求体
        payload = self._build_qwen_payload(task_name, input_data, model_config)

        # 添加额外参数
        payload.update(kwargs)

        # 发送请求
        def request_func():
            response = self._session.post(
                api_endpoint,
                headers=headers,
                json=payload,
                timeout=self.config.LLM_TIMEOUT
            )
            response.raise_for_status()
            return response.json()

        try:
            result = self._retry_request(request_func)
            return self._parse_qwen_response(result)
        except Exception as e:
            raise LLMError(f"Qwen API调用失败: {str(e)}")

    def _build_qwen_payload(self, task_name: str, input_data: Dict[str, Any],
                           model_config: Dict[str, Any]) -> Dict[str, Any]:
        """构建Qwen API请求体"""
        task_config = self._get_task_config(task_name)
        if not task_config:
            raise LLMError(f"未找到任务配置: {task_name}")

        # 获取任务提示模板
        prompt_templates = task_config.get('prompt_templates', {})
        default_prompt = prompt_templates.get('default', '')

        # 格式化提示
        formatted_prompt = self._format_prompt(default_prompt, input_data)

        # 构建消息内容
        messages = [
            {
                'role': 'user',
                'content': [
                    {
                        'type': 'text',
                        'text': formatted_prompt
                    }
                ]
            }
        ]

        # 如果输入数据包含图像，添加图像内容
        if 'image_data' in input_data:
            try:
                image_data = input_data['image_data']
                qwen_image = prepare_image_for_qwen(image_data)
                messages[0]['content'].insert(0, qwen_image)
            except ImageProcessingError as e:
                logger.warning(f"图像处理失败: {str(e)}")
                # 如果图像处理失败，继续使用文本
                pass

        payload = {
            'model': model_config.get('name', 'qwen-vl-plus'),
            'input': {
                'messages': messages
            },
            'parameters': {
                'max_tokens': model_config.get('max_tokens', 2000),
                'temperature': model_config.get('temperature', 0.1),
                'top_p': model_config.get('top_p', 0.8)
            }
        }

        return payload

    def _parse_qwen_response(self, response: Dict[str, Any]) -> Dict[str, Any]:
        """解析Qwen API响应"""
        try:
            # Qwen返回的是标准格式
            output = response.get('output', {})
            if not output:
                raise LLMResponseError("API响应为空")

            # 获取结果内容
            choices = output.get('choices', [])
            if not choices:
                raise LLMResponseError("API响应中没有choices")

            message = choices[0].get('message', {})
            content = message.get('content', '')

            # 尝试解析JSON
            try:
                parsed_content = json.loads(content.strip())
                return {
                    'success': True,
                    'model': response.get('model', ''),
                    'usage': output.get('usage', {}),
                    'result': parsed_content,
                    'raw_response': response
                }
            except json.JSONDecodeError:
                # 如果不是JSON，返回纯文本
                return {
                    'success': True,
                    'model': response.get('model', ''),
                    'usage': output.get('usage', {}),
                    'result': content,
                    'raw_response': response
                }

        except Exception as e:
            raise LLMResponseError(f"解析Qwen响应失败: {str(e)}")

    def _get_system_prompt(self, model_name: str) -> str:
        """获取系统提示"""
        # 从配置文件获取系统提示
        prompt_tuning = self._loaded_config.get('prompt_tuning', {})
        system_prompts = prompt_tuning.get('system_prompts', {})

        # 返回对应模型的系统提示
        return system_prompts.get(model_name,
                               "你是一个专业的宠物行为学家和动物健康专家。你负责分析智能宠物箱的数据，为宠物主人提供专业、准确、温暖的分析和建议。")

    def _format_prompt(self, template: str, input_data: Dict[str, Any]) -> str:
        """格式化提示模板"""
        try:
            # 替换模板中的变量
            formatted = template.format(**input_data)
            return formatted
        except KeyError as e:
            logger.warning(f"提示模板格式化错误，缺少变量: {e}")
            # 返回原始模板，不进行格式化
            return template
        except Exception as e:
            logger.warning(f"提示模板格式化错误: {e}")
            return template

    def get_model_info(self, model_name: str) -> Dict[str, Any]:
        """获取模型信息"""
        model_config = self._get_model_config(model_name)
        if not model_config:
            return {}

        return {
            'name': model_config.get('name', ''),
            'provider': model_config.get('provider', ''),
            'type': model_config.get('type', ''),
            'capabilities': model_config.get('capabilities', []),
            'endpoint': model_config.get('api_endpoint', '')
        }

    def health_check(self) -> Dict[str, Any]:
        """健康检查"""
        try:
            # 检查配置
            if not self._loaded_config:
                return {
                    'status': 'error',
                    'message': '配置文件加载失败',
                    'timestamp': datetime.utcnow().isoformat()
                }

            # 检查API密钥
            if not self._api_keys:
                return {
                    'status': 'warning',
                    'message': '未设置API密钥',
                    'timestamp': datetime.utcnow().isoformat()
                }

            # 检查主要模型是否配置
            models = self._loaded_config.get('models', {})
            if not models:
                return {
                    'status': 'warning',
                    'message': '未配置任何模型',
                    'timestamp': datetime.utcnow().isoformat()
                }

            return {
                'status': 'healthy',
                'message': 'LLM服务正常',
                'models': list(models.keys()),
                'timestamp': datetime.utcnow().isoformat()
            }

        except Exception as e:
            return {
                'status': 'error',
                'message': f'健康检查失败: {str(e)}',
                'timestamp': datetime.utcnow().isoformat()
            }

    def get_cache_stats(self) -> Dict[str, Any]:
        """获取缓存统计信息"""
        with self._cache_lock:
            return {
                'cache_size': len(self._cache),
                'cache_enabled': self.config.LLM_CACHE_ENABLED,
                'cache_ttl_seconds': self.config.LLM_CACHE_TTL
            }


# 测试函数
if __name__ == '__main__':
    # 配置日志
    logging.basicConfig(level=logging.INFO)

    print("测试LLM服务...")

    # 创建测试配置
    test_config = Config()
    test_config.LLM_ENABLED = True
    test_config.DEEPSEEK_API_KEY = "sk-e5d53f0c2ef94139b85ec53fcd585d8b"
    test_config.QWEN_API_KEY = "sk-4dd08e56abce44d781e8ad0b24fd7978"

    try:
        # 创建服务实例
        llm_service = LLMService(test_config)

        # 测试健康检查
        print("\n1. 健康检查:")
        health = llm_service.health_check()
        print(f"  结果: {health}")

        # 测试模型信息
        print("\n2. 模型信息:")
        model_info = llm_service.get_model_info('deepseek_reasoner')
        print(f"  DeepSeek: {model_info}")

        model_info = llm_service.get_model_info('qwen_vl_plus')
        print(f"  Qwen-VL-Plus: {model_info}")

        # 测试缓存统计
        print("\n3. 缓存统计:")
        cache_stats = llm_service.get_cache_stats()
        print(f"  缓存状态: {cache_stats}")

        print("\n测试完成!")

    except Exception as e:
        print(f"测试过程中发生错误: {e}")
        import traceback
        traceback.print_exc()