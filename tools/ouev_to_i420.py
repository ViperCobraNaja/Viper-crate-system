#!/usr/bin/env python3
"""O_UYY_E_VYY → I420 (YUV420p) 转换工具

用法:
  python ouev_to_i420.py dump_0.ouev              → 输出 dump_0.yuv (i420)
  python ouev_to_i420.py dump_0.ouev --png out.png → 输出 PNG

ffplay 直接播放:
  python ouev_to_i420.py dump_0.ouev --play

依赖: pip install numpy pillow
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("请先安装依赖: pip install numpy pillow")
    sys.exit(1)


def ouev_to_i420(ouev_data: bytes, w: int, h: int) -> np.ndarray:
    """将 O_UYY_E_VYY 转为 I420 (YUV420p planar).
    返回 shape (h*3//2, w) 的 uint8 数组 (Y + U + V 平面拼接).
    """
    stride = w * 3 // 2
    y_size = w * h
    uv_size = w * h // 4

    y = np.zeros(h * w, dtype=np.uint8)
    u = np.zeros(uv_size, dtype=np.uint8)
    v = np.zeros(uv_size, dtype=np.uint8)

    for row in range(0, h, 2):
        p_u = ouev_data[row * stride : (row + 1) * stride]       # 偶数行: UYYUYY...
        p_v = ouev_data[(row + 1) * stride : (row + 2) * stride]  # 奇数行: VYYVYY...

        uv_idx_base = (row // 2) * (w // 2)
        for col in range(0, w, 2):
            uv_idx = uv_idx_base + col // 2

            # 偶数行: U Y_left Y_right
            u[uv_idx] = p_u[col * 3 // 2]
            y[row * w + col] = p_u[col * 3 // 2 + 1]
            y[row * w + col + 1] = p_u[col * 3 // 2 + 2]

            # 奇数行: V Y_left Y_right
            v[uv_idx] = p_v[col * 3 // 2]
            y[(row + 1) * w + col] = p_v[col * 3 // 2 + 1]
            y[(row + 1) * w + col + 1] = p_v[col * 3 // 2 + 2]

    return np.concatenate([y, u, v])


def i420_to_rgb(i420: np.ndarray, w: int, h: int) -> np.ndarray:
    """I420 → RGB (BT.601 full range)."""
    y_size = w * h
    y_plane = i420[:y_size].reshape(h, w).astype(np.float32)
    u_plane = i420[y_size:y_size + y_size // 4]
    v_plane = i420[y_size + y_size // 4:]

    # 上采样 U/V 到全分辨率 (nearest neighbor)
    u_full = np.zeros((h, w), dtype=np.float32)
    v_full = np.zeros((h, w), dtype=np.float32)
    for r in range(0, h, 2):
        for c in range(0, w, 2):
            idx = (r // 2) * (w // 2) + c // 2
            u_full[r:r+2, c:c+2] = u_plane[idx]
            v_full[r:r+2, c:c+2] = v_plane[idx]

    y  = y_plane - 16.0
    u  = u_full - 128.0
    v  = v_full - 128.0

    r = np.clip(y + 1.40200 * v, 0, 255)
    g = np.clip(y - 0.34414 * u - 0.71414 * v, 0, 255)
    b = np.clip(y + 1.77200 * u, 0, 255)

    return np.stack([r, g, b], axis=-1).astype(np.uint8)


def main():
    parser = argparse.ArgumentParser(description="O_UYY_E_VYY → I420 converter")
    parser.add_argument("input", help="O_UYY_E_VYY raw file")
    parser.add_argument("--width", type=int, default=800, help="Image width (default: 800)")
    parser.add_argument("--height", type=int, default=800, help="Image height (default: 800)")
    parser.add_argument("--png", help="Output PNG file (auto-detected if path ends in .png)")
    parser.add_argument("--play", action="store_true", help="Print how to play with ffplay")
    parser.add_argument("-o", "--output", help="Output I420 raw file")
    args = parser.parse_args()

    data = Path(args.input).read_bytes()
    w, h = args.width, args.height
    expected = w * h * 3 // 2
    if len(data) < expected:
        print(f"Warning: file is {len(data)} bytes, expected {expected} for {w}x{h}")

    i420 = ouev_to_i420(data, w, h)

    # I420 raw output
    out_raw = args.output
    if not out_raw and not args.png and not args.play:
        out_raw = Path(args.input).with_suffix(".yuv")
    if out_raw:
        Path(out_raw).write_bytes(i420.tobytes())
        print(f"I420 saved: {out_raw}")
        print(f"ffplay -f rawvideo -pixel_format yuv420p -video_size {w}x{h} {out_raw}")

    # PNG output
    png_out = args.png
    if not png_out and Path(args.input).with_suffix(".png").exists() is False:
        pass  # only auto-PNG if explicitly requested
    if png_out:
        rgb = i420_to_rgb(i420, w, h)
        Image.fromarray(rgb).save(png_out)
        print(f"PNG saved: {png_out}")

    if args.play:
        tmp = Path(args.input).with_suffix(".yuv")
        Path(tmp).write_bytes(i420.tobytes())
        print(f"\nffplay -f rawvideo -pixel_format yuv420p -video_size {w}x{h} {tmp}")


if __name__ == "__main__":
    main()
