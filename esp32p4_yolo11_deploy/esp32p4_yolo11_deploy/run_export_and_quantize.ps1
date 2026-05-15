param(
    [string]$PythonExe = "D:\Anaconda3\envs\espdl-yolo11-snake\python.exe",
    [string]$Weights = "D:\xiangmu\viper\yolov11模型\snake_full_power_may10-3\weights\best.pt",
    [string]$CalibDir = "D:\xiangmu\viper\esp32p4_yolo11_deploy\calibration_images",
    [int]$ImgSize = 320,
    [int]$CalibSteps = 32
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $Root "output"
$Onnx = Join-Path $OutDir "snake_yolo11n_esp32p4_320.onnx"
$Espdl = Join-Path $OutDir "snake_yolo11n_esp32p4_320.espdl"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

& $PythonExe (Join-Path $Root "export_yolo11_esp_onnx.py") `
    --weights $Weights `
    --output $Onnx `
    --imgsz $ImgSize `
    --opset 13

& $PythonExe (Join-Path $Root "quantize_yolo11_esp32p4.py") `
    --onnx $Onnx `
    --output $Espdl `
    --calib-dir $CalibDir `
    --imgsz $ImgSize `
    --calib-steps $CalibSteps `
    --mixed-output-int16

Write-Host "ONNX:  $Onnx"
Write-Host "ESPDL: $Espdl"
