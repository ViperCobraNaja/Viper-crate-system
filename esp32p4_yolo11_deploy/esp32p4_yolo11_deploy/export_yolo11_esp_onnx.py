from pathlib import Path
import argparse

import onnx
import torch
from ultralytics import YOLO
from ultralytics.engine.exporter import Exporter, arange_patch, try_export
from ultralytics.nn.modules import Attention, Detect
from ultralytics.utils import LOGGER, colorstr
from ultralytics.utils.checks import check_requirements


def get_latest_opset():
    # Kept as a fallback for Ultralytics versions that no longer expose this helper.
    return 13


class ESPDetect(Detect):
    def forward(self, x):
        box0 = self.cv2[0](x[0])
        score0 = self.cv3[0](x[0])
        box1 = self.cv2[1](x[1])
        score1 = self.cv3[1](x[1])
        box2 = self.cv2[2](x[2])
        score2 = self.cv3[2](x[2])
        return box0, score0, box1, score1, box2, score2


class ESPAttention(Attention):
    def forward(self, x):
        b, c, h, w = x.shape
        n = h * w
        qkv = self.qkv(x)
        q, k, v = qkv.view(
            -1, self.num_heads, self.key_dim * 2 + self.head_dim, n
        ).split([self.key_dim, self.key_dim, self.head_dim], dim=2)
        attn = (q.transpose(-2, -1) @ k) * self.scale
        attn = attn.softmax(dim=-1)
        x = (v @ attn.transpose(-2, -1)).view(-1, c, h, w) + self.pe(
            v.reshape(-1, c, h, w)
        )
        return self.proj(x)


class ESPDetectExporter(Exporter):
    @try_export
    def export_onnx(self, prefix=colorstr("ONNX:")):
        requirements = ["onnx>=1.14.0"]
        if self.args.simplify:
            requirements += [
                "onnxsim",
                "onnxruntime" + ("-gpu" if torch.cuda.is_available() else ""),
            ]
        check_requirements(requirements)

        opset_version = self.args.opset or get_latest_opset()
        LOGGER.info(
            f"\n{prefix} starting export with onnx {onnx.__version__} opset {opset_version}..."
        )
        f = str(getattr(self, "esp_output_path", self.file.with_suffix(".onnx")))
        Path(f).parent.mkdir(parents=True, exist_ok=True)

        output_names = ["box0", "score0", "box1", "score1", "box2", "score2"]
        dynamic = self.args.dynamic
        if dynamic:
            dynamic = {"images": {0: "batch"}}
            for name in output_names:
                dynamic[name] = {0: "batch"}

        with arange_patch(self.args):
            torch.onnx.export(
                self.model,
                self.im,
                f,
                verbose=False,
                opset_version=opset_version,
                do_constant_folding=False,
                input_names=["images"],
                output_names=output_names,
                dynamic_axes=dynamic or None,
                dynamo=False,
            )

        model_onnx = onnx.load(f)
        if self.args.simplify:
            try:
                import onnxsim

                LOGGER.info(f"{prefix} simplifying with onnxsim {onnxsim.__version__}...")
                model_onnx, _ = onnxsim.simplify(model_onnx)
            except Exception as exc:
                LOGGER.warning(f"{prefix} simplifier failure: {exc}")

        for key, value in self.metadata.items():
            meta = model_onnx.metadata_props.add()
            meta.key, meta.value = key, str(value)

        onnx.save(model_onnx, f)
        return f


class ESPYOLO(YOLO):
    def export_esp_onnx(self, output, **kwargs):
        self._check_is_pytorch_model()
        custom = {
            "imgsz": self.model.args["imgsz"],
            "batch": 1,
            "data": None,
            "device": None,
            "verbose": False,
        }
        args = {**self.overrides, **custom, **kwargs, "mode": "export"}
        exporter = ESPDetectExporter(overrides=args, _callbacks=self.callbacks)
        exporter.esp_output_path = Path(output)
        return exporter(model=self.model)


def parse_args():
    parser = argparse.ArgumentParser(description="Export YOLO11 to ESP-DL friendly ONNX.")
    parser.add_argument("--weights", required=True, help="Path to trained YOLO11 .pt file.")
    parser.add_argument("--output", required=True, help="Output ONNX path.")
    parser.add_argument("--imgsz", type=int, default=320, help="Static input size.")
    parser.add_argument("--opset", type=int, default=13, help="ONNX opset.")
    parser.add_argument("--no-simplify", action="store_true", help="Disable onnxsim simplify.")
    return parser.parse_args()


def main():
    args = parse_args()
    model = ESPYOLO(args.weights)
    for module in model.modules():
        if isinstance(module, Attention):
            module.forward = ESPAttention.forward.__get__(module)
        if isinstance(module, Detect):
            module.forward = ESPDetect.forward.__get__(module)

    output = model.export_esp_onnx(
        output=args.output,
        format="onnx",
        simplify=not args.no_simplify,
        opset=args.opset,
        dynamic=False,
        imgsz=args.imgsz,
    )
    print(output)


if __name__ == "__main__":
    main()
