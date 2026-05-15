"""路由层 —— 注册所有 Flask Blueprint"""

from flask import Flask


def register_all_blueprints(app: Flask):
    """统一注册所有 API 蓝图"""
    from routes.sensors import sensors_bp
    from routes.motion import motion_bp
    from routes.videos import videos_bp
    from routes.devices import devices_bp
    from routes.reports import reports_bp
    from routes.ai import ai_bp
    from routes.regions import regions_bp

    app.register_blueprint(sensors_bp, url_prefix='/api/v1')
    app.register_blueprint(motion_bp, url_prefix='/api/v1')
    app.register_blueprint(videos_bp, url_prefix='/api/v1')
    app.register_blueprint(devices_bp, url_prefix='/api/v1')
    app.register_blueprint(reports_bp, url_prefix='/api/v1')
    app.register_blueprint(ai_bp, url_prefix='/api/v1')
    app.register_blueprint(regions_bp, url_prefix='/api/v1')
