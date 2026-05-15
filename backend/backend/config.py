"""
宠语者 - 智能宠物箱后端系统配置
"""

import os
from datetime import timedelta

class Config:
    """基础配置类"""

    # ========== Flask配置 ==========
    # 安全配置
    SECRET_KEY = os.environ.get('SECRET_KEY') or 'petbox-secret-key-change-in-production'
    SESSION_COOKIE_SECURE = False  # 生产环境设为True

    # 应用配置
    DEBUG = True
    TESTING = False

    # 服务器配置
    HOST = '0.0.0.0'
    PORT = 5001

    # ========== 数据库配置 ==========
    # MongoDB配置
    MONGO_URI = os.environ.get('MONGO_URI') or 'mongodb://localhost:27017/'
    DATABASE_NAME = 'petbox_db'

    # 连接池配置
    MONGO_MAX_POOL_SIZE = 100
    MONGO_MIN_POOL_SIZE = 10

    # ========== 数据存储配置 ==========
    # 本地存储路径
    DATA_DIR = 'data'
    SENSOR_DATA_DIR = os.path.join(DATA_DIR, 'sensor')
    VIDEO_DATA_DIR = os.path.join(DATA_DIR, 'videos')
    BACKUP_DIR = os.path.join(DATA_DIR, 'backup')

    # 数据保留策略（天）
    SENSOR_DATA_RETENTION_DAYS = 90
    VIDEO_DATA_RETENTION_DAYS = 30
    LOG_RETENTION_DAYS = 7

    # ========== 传感器配置 ==========
    # 支持的传感器类型
    SUPPORTED_SENSORS = {
        'temperature': {'min': -40, 'max': 125, 'unit': 'celsius'},
        'humidity': {'min': 0, 'max': 100, 'unit': 'percent'},
        'light': {'min': 0, 'max': 100000, 'unit': 'lux'},
        'motion': {'type': 'boolean', 'confidence_threshold': 0.7}
    }

    # 数据验证阈值
    TEMPERATURE_ALERT_THRESHOLD = {'min': 15, 'max': 30}
    HUMIDITY_ALERT_THRESHOLD = {'min': 40, 'max': 70}

    # ========== AI处理配置 ==========
    # TensorFlow Lite模型路径
    AI_MODEL_DIR = 'models'
    PET_DETECTION_MODEL = os.path.join(AI_MODEL_DIR, 'pet_detection.tflite')
    ACTIVITY_RECOGNITION_MODEL = os.path.join(AI_MODEL_DIR, 'activity_recognition.tflite')

    # AI处理参数
    PET_DETECTION_CONFIDENCE_THRESHOLD = 0.7
    ACTIVITY_DETECTION_INTERVAL = 5  # 秒

    # ========== 视频处理配置 ==========
    VIDEO_FORMATS = ['.h264', '.mp4', '.avi']
    MAX_VIDEO_SIZE = 100 * 1024 * 1024  # 100MB
    VIDEO_PREVIEW_DURATION = 10  # 预览时长（秒）

    # 视频文件存储
    VIDEO_STORAGE_DIR = os.path.join(DATA_DIR, 'videos')
    VIDEO_CONVERT_TO_MP4 = True
    VIDEO_MAX_FILE_SIZE = 200 * 1024 * 1024  # 200MB

    # 运动检测
    MOTION_EVENT_RETENTION_DAYS = 90
    MOTION_HEATMAP_GRID_COLS = 8
    MOTION_HEATMAP_GRID_ROWS = 6

    # 设备管理
    DEVICE_HEARTBEAT_INTERVAL = 300     # 5分钟
    DEVICE_OFFLINE_THRESHOLD = 600      # 10分钟无心跳视为离线
    DEVICE_COMMAND_MAX_PENDING = 20     # 每个设备最多待执行命令数

    # ========== API配置 ==========
    # CORS配置
    CORS_ORIGINS = ['http://localhost:3000', 'http://localhost:5000']

    # 请求限制
    RATE_LIMIT_PER_MINUTE = 60
    UPLOAD_MAX_CONTENT_LENGTH = 200 * 1024 * 1024  # 200MB

    # API版本
    API_VERSION = 'v1'
    API_PREFIX = f'/api/{API_VERSION}'

    # ========== 日志配置 ==========
    LOG_DIR = 'logs'
    LOG_FILE = os.path.join(LOG_DIR, 'petbox.log')
    LOG_LEVEL = 'INFO'
    LOG_FORMAT = '%(asctime)s - %(name)s - %(levelname)s - %(message)s'

    # ========== 设备配置 ==========
    # 支持的设备类型
    SUPPORTED_DEVICES = ['ESP32-P4', 'ESP32-S3', 'Raspberry-Pi']

    # 设备心跳间隔（秒）
    DEVICE_HEARTBEAT_INTERVAL = 300

    # 设备离线判定时间（秒）
    DEVICE_OFFLINE_THRESHOLD = 600

    # ========== 报告生成配置 ==========
    # 日报生成时间（UTC）
    DAILY_REPORT_GENERATION_HOUR = 23  # 每天23点生成日报

    # 报告保留天数
    REPORT_RETENTION_DAYS = 365

    # ========== 邮件通知配置 ==========
    EMAIL_ENABLED = False
    EMAIL_SMTP_SERVER = 'smtp.gmail.com'
    EMAIL_SMTP_PORT = 587
    EMAIL_SENDER = 'petbox@example.com'

    # 通知阈值
    TEMPERATURE_ALERT_ENABLED = True
    HUMIDITY_ALERT_ENABLED = True
    DEVICE_OFFLINE_ALERT_ENABLED = True

    # ========== 缓存配置 ==========
    CACHE_ENABLED = True
    CACHE_TIMEOUT = 300  # 5分钟

    # ========== 定时任务配置 ==========
    SCHEDULER_ENABLED = True
    SCHEDULER_INTERVAL = 60  # 秒

    # ========== LLM 配置 ==========
    # API密钥配置
    DEEPSEEK_API_KEY = os.environ.get('DEEPSEEK_API_KEY', '')
    QWEN_API_KEY = os.environ.get('QWEN_API_KEY', '')

    # LLM服务开关
    LLM_ENABLED = True
    LLM_CONFIG_PATH = 'llm_config.json'

    # API调用参数
    LLM_TIMEOUT = 30  # 秒
    LLM_MAX_RETRIES = 3
    LLM_RETRY_DELAY = 1  # 秒
    LLM_BACKOFF_FACTOR = 2

    # 缓存配置
    LLM_CACHE_ENABLED = True
    LLM_CACHE_TTL = 300  # 秒

    # 降级策略
    LLM_FALLBACK_ENABLED = True
    LLM_FALLBACK_TO_RULES = True
    LLM_FALLBACK_TO_MOCK = False  # 完全替换模拟，所以通常禁用

    # 监控配置
    LLM_METRICS_ENABLED = True
    LLM_MAX_LATENCY_SECONDS = 10
    LLM_MIN_SUCCESS_RATE = 0.95

class DevelopmentConfig(Config):
    """开发环境配置"""
    DEBUG = True
    MONGO_URI = 'mongodb://localhost:27017/petbox_dev'

    # 开发环境LLM配置
    LLM_ENABLED = True
    LLM_FALLBACK_TO_MOCK = False  # 开发环境也使用真实LLM

class TestingConfig(Config):
    """测试环境配置"""
    TESTING = True
    DEBUG = True
    MONGO_URI = 'mongodb://localhost:27017/petbox_test'
    SECRET_KEY = 'testing-secret-key'

class ProductionConfig(Config):
    """生产环境配置"""
    DEBUG = False
    TESTING = False

    # 生产环境安全配置
    SECRET_KEY = os.environ.get('SECRET_KEY')
    SESSION_COOKIE_SECURE = True

    # 生产数据库
    MONGO_URI = os.environ.get('MONGO_URI')

    # 生产日志配置
    LOG_LEVEL = 'WARNING'

    # 生产环境LLM配置
    LLM_ENABLED = True
    LLM_FALLBACK_TO_RULES = True  # 生产环境开启规则降级
    LLM_MAX_LATENCY_SECONDS = 8   # 生产环境要求更高性能

# 配置映射
config_by_name = {
    'development': DevelopmentConfig,
    'testing': TestingConfig,
    'production': ProductionConfig,
    'default': DevelopmentConfig
}

def get_config(config_name: str = None) -> Config:
    """获取配置对象"""
    if config_name is None:
        config_name = os.environ.get('FLASK_ENV', 'default')

    return config_by_name.get(config_name, config_by_name['default'])

# 当前使用的配置
current_config = get_config()

# 打印配置信息（调试用）
if __name__ == '__main__':
    config = current_config
    print(f"当前配置: {config.__class__.__name__}")
    print(f"MongoDB URI: {config.MONGO_URI}")
    print(f"调试模式: {config.DEBUG}")
    print(f"API版本: {config.API_VERSION}")
    print(f"数据目录: {config.DATA_DIR}")