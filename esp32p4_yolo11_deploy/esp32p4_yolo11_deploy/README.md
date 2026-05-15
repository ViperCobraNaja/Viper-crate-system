# ESP32-P4 YOLO11 Snake Model Deployment

## 中文说明

本目录用于把训练好的 YOLO11 蛇类检测模型转换为 ESP32-P4 可部署的 ESP-DL 模型。流程参考乐鑫官方 YOLO11n 部署文档：

- https://docs.espressif.com/projects/esp-dl/zh_CN/latest/tutorials/how_to_deploy_yolo11n.html
- https://docs.espressif.com/projects/esp-dl/zh_CN/latest/getting_started/readme.html

### 1. 项目背景

宠语者项目使用 ESP32-P4 做端侧感知：先由硬件运动矢量触发候选事件，再调用本地 AI 模型确认画面中是否真的出现宠物。确认成功后，系统进入录像、事件记录和日报统计流程。

因此本模型部署目标是：

- 单 batch 推理：`batch=1`
- 固定输入尺寸：默认 `320x320`
- 目标平台：`esp32p4`
- 模型格式：`.espdl`
- 检测类别：`snake`
- 用途：运动触发后的本地蛇类目标确认

### 2. 目录结构

```text
D:\xiangmu\viper\esp32p4_yolo11_deploy
|-- README.md
|-- environment.yml
|-- export_yolo11_esp_onnx.py
|-- quantize_yolo11_esp32p4.py
|-- run_export_and_quantize.ps1
|-- calibration_images\
|-- output\
    |-- snake_yolo11n_esp32p4_320.onnx
    |-- snake_yolo11n_esp32p4_320.espdl
    |-- snake_yolo11n_esp32p4_320.json
    |-- snake_yolo11n_esp32p4_320.info
```

文件作用：

- `export_yolo11_esp_onnx.py`：把 YOLO11 `.pt` 权重导出为 ESP-DL 友好的 6 输出 ONNX。
- `quantize_yolo11_esp32p4.py`：使用 ESP-PPQ 把 ONNX 量化并导出 `.espdl`。
- `run_export_and_quantize.ps1`：一键执行导出和量化。
- `environment.yml`：Conda 环境依赖清单。
- `calibration_images`：量化校准图片目录。
- `output`：最终产物目录。

### 3. 当前已生成产物

```text
D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.onnx
D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.espdl
D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.json
D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.info
```

已验证的 ONNX 信息：

```text
opset: 13
input: images [1, 3, 320, 320]
outputs: box0, score0, box1, score1, box2, score2
```

`snake_yolo11n_esp32p4_320.espdl` 是给 ESP32-P4 端部署使用的模型文件。

### 4. Conda 环境

本机原先 `conda` 不在 PATH 中，所以已在项目内安装本地 Miniconda：

```powershell
D:\xiangmu\viper\.tools\miniconda3
```

已创建环境：

```powershell
espdl-yolo11-snake
```

由于本机 Conda 配置，环境实际路径为：

```powershell
D:\Anaconda3\envs\espdl-yolo11-snake
```

脚本默认直接调用该环境的 Python，避免 Windows 下 `conda run` 打印 ESP-PPQ 日志时出现 GBK 编码错误：

```powershell
D:\Anaconda3\envs\espdl-yolo11-snake\python.exe
```

如果在另一台机器上重建环境：

```powershell
conda env create -f D:\xiangmu\viper\esp32p4_yolo11_deploy\environment.yml
conda activate espdl-yolo11-snake
```

如果你希望脚本使用另一台机器的 Python，运行时传入 `-PythonExe`：

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1 `
  -PythonExe "C:\path\to\envs\espdl-yolo11-snake\python.exe"
```

### 5. 一键重新生成模型

直接运行：

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1
```

默认使用：

```text
weights: D:\xiangmu\viper\yolov11模型\snake_full_power_may10-3\weights\best.pt
calibration images: D:\xiangmu\viper\esp32p4_yolo11_deploy\calibration_images
image size: 320
calibration steps: 32
target: esp32p4
quantization: INT8
mixed precision: final detection heads use INT16
```

运行完成后查看：

```powershell
Get-ChildItem D:\xiangmu\viper\esp32p4_yolo11_deploy\output
```

### 6. 替换训练好的模型

假设新的 YOLO11 权重在：

```text
D:\new_model\weights\best.pt
```

一键脚本替换 `-Weights` 参数：

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1 `
  -Weights "D:\new_model\weights\best.pt"
```

如果新模型仍是 YOLO11 检测模型，脚本会自动做官方部署文档中的 Detect 输出改造，把 YOLO11 原始检测头导出为 6 个输出：

```text
box0, score0, box1, score1, box2, score2
```

注意事项：

- 新模型必须是 Ultralytics YOLO11 检测任务的 `.pt` 模型。
- 如果类别不再是 `snake`，需要同步修改 `quantize_yolo11_esp32p4.py` 中 `metadata` 的 `classes` 字段。
- 如果输入尺寸不是 `320`，运行脚本时传入 `-ImgSize`。

例如导出 `640x640`：

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1 `
  -Weights "D:\new_model\weights\best.pt" `
  -ImgSize 640
```

ESP32-P4 端侧算力和内存有限，宠语者当前建议优先使用 `320`。

### 7. 替换量化校准数据

量化校准数据会影响 `.espdl` 的实际精度。当前 `calibration_images` 中使用的是训练结果图和验证图的副本，只适合作为流程验证。正式部署建议换成宠物箱摄像头采集的真实帧。

推荐校准图片要求：

- 来自实际宠物箱摄像头，例如 SC2336。
- 覆盖白天、夜间、补光、阴影、不同背景。
- 同时包含有蛇和无蛇画面。
- 建议至少 50 到 200 张。
- 图片格式可用 `.jpg`、`.jpeg`、`.png`、`.bmp`、`.webp`。
- 路径尽量使用英文或数字，避免 Windows 工具链中文路径乱码。

推荐目录示例：

```text
D:\pet_talker_calib\snake_box_frames
```

运行：

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1 `
  -CalibDir "D:\pet_talker_calib\snake_box_frames" `
  -CalibSteps 64
```

如果校准图片数量少于 `CalibSteps`，脚本会自动使用实际图片数量。

### 8. 分步运行

如果需要调试，可以分两步执行。

第一步：导出 ESP-DL 友好的 ONNX。

```powershell
& D:\Anaconda3\envs\espdl-yolo11-snake\python.exe `
  D:\xiangmu\viper\esp32p4_yolo11_deploy\export_yolo11_esp_onnx.py `
  --weights "D:\xiangmu\viper\yolov11模型\snake_full_power_may10-3\weights\best.pt" `
  --output "D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.onnx" `
  --imgsz 320 `
  --opset 13
```

第二步：量化并导出 `.espdl`。

```powershell
& D:\Anaconda3\envs\espdl-yolo11-snake\python.exe `
  D:\xiangmu\viper\esp32p4_yolo11_deploy\quantize_yolo11_esp32p4.py `
  --onnx "D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.onnx" `
  --output "D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.espdl" `
  --calib-dir "D:\xiangmu\viper\esp32p4_yolo11_deploy\calibration_images" `
  --imgsz 320 `
  --calib-steps 32 `
  --mixed-output-int16
```

### 9. 参数说明

`run_export_and_quantize.ps1` 参数：

```text
-PythonExe    Python 解释器路径
-Weights      YOLO11 .pt 权重路径
-CalibDir     量化校准图片目录
-ImgSize      输入尺寸，默认 320
-CalibSteps   量化校准步数，默认 32
```

`export_yolo11_esp_onnx.py` 参数：

```text
--weights      YOLO11 .pt 权重路径
--output       输出 ONNX 路径
--imgsz        输入尺寸
--opset        ONNX opset，默认 13
--no-simplify  不使用 onnxsim 简化模型
```

`quantize_yolo11_esp32p4.py` 参数：

```text
--onnx                 输入 ONNX 路径
--output               输出 .espdl 路径
--calib-dir            校准图片目录
--imgsz                输入尺寸
--calib-steps          校准步数
--device               cpu 或 cuda
--mixed-output-int16   将最终检测头设为 INT16，降低输出层量化误差
--no-error-report      跳过 ESP-PPQ 误差报告
--export-test-values   导出测试张量
```

### 10. 验证 ONNX

运行：

```powershell
& D:\Anaconda3\envs\espdl-yolo11-snake\python.exe -c "import onnx; p=r'D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.onnx'; m=onnx.load(p); onnx.checker.check_model(m); print([(o.domain,o.version) for o in m.opset_import]); print([o.name for o in m.graph.output])"
```

期望输出包含：

```text
[('', 13)]
['box0', 'score0', 'box1', 'score1', 'box2', 'score2']
```

### 11. 常见问题

问题：`conda` 找不到。

处理：使用完整路径调用本地 Conda，或直接使用环境 Python：

```powershell
D:\xiangmu\viper\.tools\miniconda3\Scripts\conda.exe
D:\Anaconda3\envs\espdl-yolo11-snake\python.exe
```

问题：`conda run` 出现 GBK 编码错误。

处理：不要用 `conda run` 执行量化脚本，直接调用环境内的 `python.exe`。

问题：OpenCV 读取中文路径图片失败。

处理：把校准图片复制到英文路径，例如：

```text
D:\pet_talker_calib\snake_box_frames
```

问题：更换模型后类别名称不对。

处理：修改 `quantize_yolo11_esp32p4.py` 中：

```python
"classes": "snake"
```

问题：模型太大或 ESP32-P4 推理太慢。

处理：优先使用 `320x320` 输入；减少类别数量；使用更小的 YOLO11n 模型；用更贴近真实场景的校准图片重跑量化。

---

## English Guide

This folder converts a trained YOLO11 snake detector into an ESP-DL model deployable on ESP32-P4. The flow follows Espressif's official YOLO11n ESP-DL deployment guide:

- https://docs.espressif.com/projects/esp-dl/zh_CN/latest/tutorials/how_to_deploy_yolo11n.html
- https://docs.espressif.com/projects/esp-dl/zh_CN/latest/getting_started/readme.html

### 1. Project Context

Pet Talker uses ESP32-P4 for edge perception. Hardware motion vectors first trigger a candidate event, then the local AI model confirms whether the pet is present. When detection succeeds, the system records video, logs the event, and contributes to daily behavior summaries.

Deployment targets:

- single-batch inference: `batch=1`
- static input size: default `320x320`
- target chip: `esp32p4`
- output model format: `.espdl`
- class: `snake`
- purpose: local confirmation after motion-triggered events

### 2. Folder Layout

```text
D:\xiangmu\viper\esp32p4_yolo11_deploy
|-- README.md
|-- environment.yml
|-- export_yolo11_esp_onnx.py
|-- quantize_yolo11_esp32p4.py
|-- run_export_and_quantize.ps1
|-- calibration_images\
|-- output\
    |-- snake_yolo11n_esp32p4_320.onnx
    |-- snake_yolo11n_esp32p4_320.espdl
    |-- snake_yolo11n_esp32p4_320.json
    |-- snake_yolo11n_esp32p4_320.info
```

File roles:

- `export_yolo11_esp_onnx.py`: exports a YOLO11 `.pt` model to an ESP-DL-friendly 6-output ONNX model.
- `quantize_yolo11_esp32p4.py`: quantizes the ONNX model with ESP-PPQ and exports `.espdl`.
- `run_export_and_quantize.ps1`: runs export and quantization in one command.
- `environment.yml`: Conda dependency file.
- `calibration_images`: calibration images for quantization.
- `output`: generated deployment artifacts.

### 3. Current Artifacts

```text
D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.onnx
D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.espdl
D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.json
D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.info
```

Verified ONNX metadata:

```text
opset: 13
input: images [1, 3, 320, 320]
outputs: box0, score0, box1, score1, box2, score2
```

Use `snake_yolo11n_esp32p4_320.espdl` on the ESP32-P4 side.

### 4. Conda Environment

This machine did not have `conda` on PATH, so a local Miniconda was installed here:

```powershell
D:\xiangmu\viper\.tools\miniconda3
```

Created environment:

```powershell
espdl-yolo11-snake
```

Actual environment path on this machine:

```powershell
D:\Anaconda3\envs\espdl-yolo11-snake
```

The script calls the environment Python directly to avoid Windows `conda run` GBK log-output issues:

```powershell
D:\Anaconda3\envs\espdl-yolo11-snake\python.exe
```

To recreate the environment on another machine:

```powershell
conda env create -f D:\xiangmu\viper\esp32p4_yolo11_deploy\environment.yml
conda activate espdl-yolo11-snake
```

If your Python path is different, pass `-PythonExe`:

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1 `
  -PythonExe "C:\path\to\envs\espdl-yolo11-snake\python.exe"
```

### 5. Build the Model

Run:

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1
```

Defaults:

```text
weights: D:\xiangmu\viper\yolov11模型\snake_full_power_may10-3\weights\best.pt
calibration images: D:\xiangmu\viper\esp32p4_yolo11_deploy\calibration_images
image size: 320
calibration steps: 32
target: esp32p4
quantization: INT8
mixed precision: final detection heads use INT16
```

Check outputs:

```powershell
Get-ChildItem D:\xiangmu\viper\esp32p4_yolo11_deploy\output
```

### 6. Replace the Trained Model

Assume the new YOLO11 weights are:

```text
D:\new_model\weights\best.pt
```

Run:

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1 `
  -Weights "D:\new_model\weights\best.pt"
```

For YOLO11 detection models, the export script automatically applies the ESP-DL Detect-head change and produces six outputs:

```text
box0, score0, box1, score1, box2, score2
```

Notes:

- The new model should be an Ultralytics YOLO11 detection `.pt` model.
- If the class is no longer `snake`, update the `classes` field in `quantize_yolo11_esp32p4.py`.
- If the input size changes, pass `-ImgSize`.

Example for `640x640`:

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1 `
  -Weights "D:\new_model\weights\best.pt" `
  -ImgSize 640
```

For ESP32-P4 deployment, `320x320` is recommended unless accuracy requires a larger input.

### 7. Replace Calibration Data

Calibration data affects the final `.espdl` accuracy. The current `calibration_images` folder is good enough for pipeline validation, but production deployment should use real camera frames from the pet enclosure.

Recommended calibration images:

- captured by the actual enclosure camera, such as SC2336
- include day, night, fill light, shadows, and background variations
- include both snake-present and snake-absent frames
- preferably 50 to 200 images
- supported formats: `.jpg`, `.jpeg`, `.png`, `.bmp`, `.webp`
- use ASCII paths when possible to avoid Windows path encoding issues

Example:

```text
D:\pet_talker_calib\snake_box_frames
```

Run:

```powershell
& D:\xiangmu\viper\esp32p4_yolo11_deploy\run_export_and_quantize.ps1 `
  -CalibDir "D:\pet_talker_calib\snake_box_frames" `
  -CalibSteps 64
```

If the number of images is smaller than `CalibSteps`, the script uses the actual image count.

### 8. Run Step by Step

Export ONNX:

```powershell
& D:\Anaconda3\envs\espdl-yolo11-snake\python.exe `
  D:\xiangmu\viper\esp32p4_yolo11_deploy\export_yolo11_esp_onnx.py `
  --weights "D:\xiangmu\viper\yolov11模型\snake_full_power_may10-3\weights\best.pt" `
  --output "D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.onnx" `
  --imgsz 320 `
  --opset 13
```

Quantize and export `.espdl`:

```powershell
& D:\Anaconda3\envs\espdl-yolo11-snake\python.exe `
  D:\xiangmu\viper\esp32p4_yolo11_deploy\quantize_yolo11_esp32p4.py `
  --onnx "D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.onnx" `
  --output "D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.espdl" `
  --calib-dir "D:\xiangmu\viper\esp32p4_yolo11_deploy\calibration_images" `
  --imgsz 320 `
  --calib-steps 32 `
  --mixed-output-int16
```

### 9. Parameters

`run_export_and_quantize.ps1`:

```text
-PythonExe    Python executable path
-Weights      YOLO11 .pt weights path
-CalibDir     calibration image directory
-ImgSize      input size, default 320
-CalibSteps   calibration steps, default 32
```

`export_yolo11_esp_onnx.py`:

```text
--weights      YOLO11 .pt weights path
--output       output ONNX path
--imgsz        input size
--opset        ONNX opset, default 13
--no-simplify  disable onnxsim simplification
```

`quantize_yolo11_esp32p4.py`:

```text
--onnx                 input ONNX path
--output               output .espdl path
--calib-dir            calibration image directory
--imgsz                input size
--calib-steps          calibration steps
--device               cpu or cuda
--mixed-output-int16   use INT16 for final detection heads
--no-error-report      skip ESP-PPQ error reports
--export-test-values   export test tensors
```

### 10. Validate ONNX

Run:

```powershell
& D:\Anaconda3\envs\espdl-yolo11-snake\python.exe -c "import onnx; p=r'D:\xiangmu\viper\esp32p4_yolo11_deploy\output\snake_yolo11n_esp32p4_320.onnx'; m=onnx.load(p); onnx.checker.check_model(m); print([(o.domain,o.version) for o in m.opset_import]); print([o.name for o in m.graph.output])"
```

Expected:

```text
[('', 13)]
['box0', 'score0', 'box1', 'score1', 'box2', 'score2']
```

### 11. Troubleshooting

Issue: `conda` is not found.

Fix: use full paths:

```powershell
D:\xiangmu\viper\.tools\miniconda3\Scripts\conda.exe
D:\Anaconda3\envs\espdl-yolo11-snake\python.exe
```

Issue: `conda run` fails with a GBK encoding error.

Fix: call the environment `python.exe` directly.

Issue: OpenCV fails to read images under Chinese paths.

Fix: move calibration images to an ASCII path, for example:

```text
D:\pet_talker_calib\snake_box_frames
```

Issue: class metadata is wrong after replacing the model.

Fix: update this field in `quantize_yolo11_esp32p4.py`:

```python
"classes": "snake"
```

Issue: model is too slow or too large for ESP32-P4.

Fix: prefer `320x320`, use a small YOLO11n model, keep the class count small, and recalibrate using real enclosure frames.
