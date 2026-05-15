"""视频路由 [重写] —— 文件上传下载 + 元数据管理"""

import os
import logging
from datetime import datetime, timezone
from flask import Blueprint, request, jsonify, current_app, send_file

logger = logging.getLogger(__name__)
videos_bp = Blueprint('videos', __name__)


@videos_bp.route('/video/upload', methods=['POST'])
def upload_video():
    """设备上传视频文件（multipart: file + metadata JSON）

    接收: multipart/form-data
      - file: H.264 视频文件
      - metadata: JSON 字符串（video_metadata_t 格式）
    """
    try:
        if 'file' not in request.files:
            return jsonify({"error": "缺少视频文件"}), 400

        file = request.files['file']
        device_id = request.form.get('device_id', 'unknown')
        metadata_str = request.form.get('metadata', '{}')

        import json
        try:
            metadata = json.loads(metadata_str)
        except json.JSONDecodeError:
            metadata = {}

        file_data = file.read()
        if len(file_data) == 0:
            return jsonify({"error": "文件为空"}), 400

        result = current_app.video_service.receive_upload(
            device_id=device_id,
            file_data=file_data,
            filename=file.filename,
            metadata_dict=metadata,
        )

        return jsonify({
            "message": "视频上传成功",
            "video_id": result.video_id,
            "original_filename": result.original_filename,
            "converted_to_mp4": result.converted,
            "file_size": len(file_data),
            "metadata": result.metadata.to_dict(),
        }), 201
    except Exception as e:
        logger.error(f"视频上传失败: {e}")
        return jsonify({"error": "内部服务器错误", "details": str(e)}), 500


@videos_bp.route('/video/metadata', methods=['POST'])
def upload_video_metadata():
    """上传视频元数据（兼容旧版仅传元数据的场景）"""
    try:
        metadata = request.get_json()
        if not metadata:
            return jsonify({"error": "缺少元数据"}), 400

        metadata["upload_time"] = datetime.now(timezone.utc).isoformat()
        result = current_app.storage_manager.save_video_metadata(metadata)

        return jsonify({
            "message": "视频元数据上传成功",
            "video_id": metadata.get("video_id"),
            "filename": metadata.get("filename"),
            "storage_result": result
        }), 201
    except Exception as e:
        logger.error(f"上传视频元数据时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@videos_bp.route('/video/list', methods=['GET'])
def list_videos():
    """获取视频列表"""
    try:
        device_id = request.args.get('device_id')
        start_time = request.args.get('start_time')
        end_time = request.args.get('end_time')

        # 优先从 MongoDB 获取
        db = current_app.storage_manager.db_storage
        if db:
            query = {}
            if device_id:
                query["device_id"] = device_id
            if start_time or end_time:
                query["create_time"] = {}
                if start_time:
                    query["create_time"]["$gte"] = start_time
                if end_time:
                    query["create_time"]["$lte"] = end_time
            from pymongo import DESCENDING
            cursor = db.video_metadata.find(query).sort(
                "create_time", DESCENDING).limit(50)
            videos = []
            for doc in cursor:
                doc['_id'] = str(doc['_id'])
                videos.append(doc)
        else:
            videos = []

        return jsonify({"count": len(videos), "videos": videos}), 200
    except Exception as e:
        logger.error(f"获取视频列表时出错: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@videos_bp.route('/video/<video_id>/play', methods=['GET'])
def play_video(video_id):
    """播放视频 —— 返回 MP4 文件流"""
    try:
        device_id = request.args.get('device_id', 'unknown')
        file_path = current_app.video_service.get_video_path(
            device_id, video_id)

        if not file_path or not os.path.exists(file_path):
            return jsonify({"error": "视频文件未找到"}), 404

        return send_file(file_path, mimetype='video/mp4',
                         as_attachment=False,
                         download_name=f"{video_id}.mp4")
    except Exception as e:
        logger.error(f"播放视频失败: {e}")
        return jsonify({"error": "内部服务器错误"}), 500


@videos_bp.route('/video/<video_id>', methods=['DELETE'])
def delete_video(video_id):
    """删除视频"""
    try:
        device_id = request.args.get('device_id', 'unknown')
        deleted = current_app.video_service.delete_video(device_id, video_id)

        if deleted:
            return jsonify({"message": f"视频 {video_id} 已删除"}), 200
        else:
            return jsonify({"error": "视频文件未找到"}), 404
    except Exception as e:
        logger.error(f"删除视频失败: {e}")
        return jsonify({"error": "内部服务器错误"}), 500
