from pathlib import Path
import argparse

import cv2
import numpy as np
import torch
from esp_ppq.api import QuantizationSettingFactory, espdl_quantize_onnx
from esp_ppq.core import TargetPlatform
from torch.utils.data import DataLoader, Dataset


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


class CalibrationImages(Dataset):
    def __init__(self, image_dir, size):
        self.image_dir = Path(image_dir)
        self.size = size
        self.images = [
            path
            for path in sorted(self.image_dir.rglob("*"))
            if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES
        ]
        if not self.images:
            raise FileNotFoundError(f"No calibration images found under {self.image_dir}")

    def __len__(self):
        return len(self.images)

    def __getitem__(self, index):
        image = cv2.imread(str(self.images[index]))
        if image is None:
            raise ValueError(f"Failed to read calibration image: {self.images[index]}")
        image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        image = cv2.resize(image, (self.size, self.size), interpolation=cv2.INTER_LINEAR)
        image = image.astype(np.float32) / 255.0
        image = np.transpose(image, (2, 0, 1))
        return torch.from_numpy(image)


def parse_args():
    parser = argparse.ArgumentParser(description="Quantize ESP-DL YOLO11 ONNX for ESP32-P4.")
    parser.add_argument("--onnx", required=True, help="Input 6-output ONNX path.")
    parser.add_argument("--output", required=True, help="Output .espdl path.")
    parser.add_argument("--calib-dir", required=True, help="Calibration image directory.")
    parser.add_argument("--imgsz", type=int, default=320, help="Static input size.")
    parser.add_argument("--calib-steps", type=int, default=32, help="Calibration steps.")
    parser.add_argument("--device", default="cpu", help="Quantization device.")
    parser.add_argument(
        "--mixed-output-int16",
        action="store_true",
        help="Use official mixed precision hint: make final detection heads INT16.",
    )
    parser.add_argument(
        "--no-error-report",
        action="store_true",
        help="Skip graphwise/layerwise error reports.",
    )
    parser.add_argument(
        "--export-test-values",
        action="store_true",
        help="Export test tensors next to the .espdl file.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    dataset = CalibrationImages(args.calib_dir, args.imgsz)
    calib_steps = min(args.calib_steps, len(dataset))
    dataloader = DataLoader(dataset, batch_size=1, shuffle=False)

    setting = QuantizationSettingFactory.espdl_setting()
    if args.mixed_output_int16:
        for op_name in [
            "/model.23/cv2.0/cv2.0.2/Conv",
            "/model.23/cv3.0/cv3.0.2/Conv",
            "/model.23/cv2.1/cv2.1.2/Conv",
            "/model.23/cv3.1/cv3.1.2/Conv",
            "/model.23/cv2.2/cv2.2.2/Conv",
            "/model.23/cv3.2/cv3.2.2/Conv",
        ]:
            setting.dispatching_table.append(op_name, TargetPlatform.ESPDL_INT16)

    metadata = {
        "model_name": "pet_talker_snake_yolo11n",
        "classes": "snake",
        "input_shape": f"1,3,{args.imgsz},{args.imgsz}",
        "target": "esp32p4",
        "quantization": "int8",
    }

    espdl_quantize_onnx(
        onnx_import_file=args.onnx,
        espdl_export_file=args.output,
        calib_dataloader=dataloader,
        calib_steps=calib_steps,
        input_shape=[1, 3, args.imgsz, args.imgsz],
        target="esp32p4",
        num_of_bits=8,
        setting=setting,
        device=args.device,
        error_report=not args.no_error_report,
        export_config=True,
        export_test_values=args.export_test_values,
        metadata_props=metadata,
    )
    print(args.output)


if __name__ == "__main__":
    main()
