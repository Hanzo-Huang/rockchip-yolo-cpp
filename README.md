# Rockchip YOLO C++

YOLO inference on **RK3576 and RK3588**, with a C++ engine and a web dashboard.
Uses **MPP** for video decoding, **RGA** for preprocessing, and **RKNN** for NPU inference.

[Download](https://github.com/Hanzo-Huang/rockchip-yolo-cpp/releases) ·
[Build from source](#build-from-source) ·
[Use the engine](#use-the-engine) ·
[Report an issue](https://github.com/Hanzo-Huang/rockchip-yolo-cpp/issues)

![Dashboard with video playback and YOLO detections](assets/screenshots/dashboard-running.jpg)

- RTSP streams, video files, and H.264 V4L2 cameras.
- Detection, instance masks, keypoints, rotated boxes, classification, semantic maps, and depth overlays.
- Video playback, recording, and model/video uploads.
- Automatic parallel inference: two NPU workers on RK3576, three on RK3588.
- RGA and CPU preprocessing modes for comparison; both use MPP and RKNN.

## Quick start

Use a board image with working **MPP, RGA, and RKNPU drivers**. The prebuilt
package targets **Ubuntu 26.04 ARM64** and includes the application, shared
libraries, and web frontend. Models are supplied separately.

Download the runtime archive and `SHA256SUMS` from
[Releases](https://github.com/Hanzo-Huang/rockchip-yolo-cpp/releases), then run on the board:

```bash
sha256sum --check SHA256SUMS
tar -xzf rockchip-yolo-cpp-v0.3.0-ubuntu26.04-arm64.tar.gz
cd rockchip-yolo-cpp
./nvr run
```

Open **http://BOARD_IP:8080** in your browser. To change the port, use
`./nvr run --port 8088`. Press **Ctrl-C** to stop.

If `ffprobe` is missing, install it with `sudo apt install ffmpeg`.
Keep the extracted directory together; the executables need its libraries and web assets.

### Start a channel

1. In **Model warehouse**, choose **Import from Hugging Face**, or upload a `.rknn` model and its `metadata.yaml` (or a ZIP containing both).
2. For a video file, upload an MP4 in **Video warehouse**. For a camera or RTSP stream, select it when adding a channel.
3. In **Dashboard**, choose **Add channel**, select the source and model, then **Start**.

Use **H.264** for live browser playback. The engine also decodes 8-bit H.265,
but browser playback depends on codec support.

**Run on a trusted LAN. There is no authentication.**

## Models and performance

### Preconverted RKNN models

Download board-specific builds from [HanzoHuang on Hugging Face](https://huggingface.co/HanzoHuang/models?search=RKNN2).
Start with [YOLO26](https://huggingface.co/HanzoHuang/YOLO26-RKNN2-Ultralytics),
[YOLO11](https://huggingface.co/HanzoHuang/YOLO11-RKNN2-Ultralytics),
[YOLOv8](https://huggingface.co/HanzoHuang/YOLOv8-RKNN2-Ultralytics), or
[YOLOv10](https://huggingface.co/HanzoHuang/YOLOv10-RKNN2-Ultralytics).
Open **Files and versions** and choose the toolkit version, task, model size,
chip, and precision. For example, this is the
[YOLO26n segmentation model for RK3576, INT8](https://huggingface.co/HanzoHuang/YOLO26-RKNN2-Ultralytics/blob/main/models/v2.3.2/segment/yolo26n-seg/rk3576/int8/model.rknn).

In **Model warehouse → Import from Hugging Face**, paste the public `.rknn` file
link. The board downloads it and the adjacent `metadata.yaml`, then checks
compatibility. A public ZIP containing both files also works. The board needs
internet access and `curl` for direct imports. For offline use, download the
pair from the same folder and upload it in Model warehouse.

For detection, instance segmentation, pose, and oriented boxes, open
**Detection classes** in channel settings to choose which classes appear in
the results. The model still runs on each inference frame.

### Model compatibility

Choose the export for your board's chip. RK3576 and RK3588 need their own
`.rknn` files. The metadata must match the model's input size and class names.

The application supports both **Ultralytics RKNN exports** and **Rockchip Model
Zoo RKNN outputs**. Support depends on the output tensors and model metadata.
The tested model families and tasks are:

| Family | Tasks | Export paths tested |
| --- | --- | --- |
| YOLOv5 | Detection, instance segmentation, classification | Model Zoo detection and segmentation, custom classification |
| YOLOv6, YOLOv7, YOLOX, PP-YOLOE | Detection | Model Zoo or its linked Rockchip exporter |
| YOLOv8, YOLO11 | Detection, instance segmentation, pose, oriented boxes, classification | Ultralytics, Model Zoo or Rockchip-guided spatial heads, custom classification |
| YOLOv10 | Detection | Ultralytics and Rockchip |
| YOLO26 | Detection, instance segmentation, pose, oriented boxes, classification, semantic segmentation, depth | Ultralytics and custom task exports, plus Model Zoo detection |

Use a checkpoint trained for the task you need. Models require one batch-1 RGB
input and matching `metadata.yaml`. The app accepts decoded and end-to-end
outputs, as well as supported Rockchip split and spatial heads. YOLO-World's
detector also needs a text input from a separate CLIP encoder. Those models
were tested with Rockchip's demo and require that demo to run.
Inspect a model with `./nvr inspect /path/to/model.rknn`.

### Convert a model

For **Ultralytics outputs**, follow the [RKNN export guide](https://docs.ultralytics.com/integrations/rockchip-rknn/)
and keep the exported metadata. For **Model Zoo outputs**, follow the
[model's conversion instructions](https://github.com/airockchip/rknn_model_zoo#model-support)
and include a metadata sidecar with the task, batch size, image size, and labels.
Select the target chip during conversion. Upload both files to Model Warehouse.

For Model Zoo exports, set `head: anchored` for YOLOv5 and YOLOv7 and copy the
three rows of width and height pairs from the example's anchor file into
`anchors`. Use `head: yolox` for YOLOX and `end2end: true` for YOLOv10 and
YOLO26 detection. For COCO pose models, use `kpt_shape: [17, 3]`.

Classification uses a center crop, depth fills the model input, and other tasks
use letterboxing by default. For an existing YOLO26 depth export with
`preprocess: letterbox` in its sidecar, change it to `preprocess: stretch` and
re-import the model to match Ultralytics. For INT8 exports, use the same resize
policy during calibration.

JSON results contain `detections`, with optional `mask`, `keypoints`, or `polygon` per object;
classification uses `classification`, and semantic/depth models use `dense`.
Raster maps are reduced for preview and encoded as alternating value/count runs.
Depth values decode as `offset + value * scale`; 65536 marks an invalid sample.

Single-channel tests with **YOLO26n, 640×640 input, RGA preprocessing, and a
4K 60 FPS H.264 RTSP stream** reached approximately **60 inference FPS on both boards**.
Throughput depends on the model and concurrent workload.

The dashboard reports decode and inference FPS separately from browser playback.
When inference falls behind, older queued frames are dropped so playback can continue.

## Build from source

On the board:

```bash
git clone https://github.com/Hanzo-Huang/rockchip-yolo-cpp.git
cd rockchip-yolo-cpp
./nvr build
./nvr run
```

The build command prepares dependencies, builds C++ with CMake, and builds the
React frontend. The first run needs internet access and may request sudo to
install packages. It does not install kernel drivers.

| Command | Purpose |
| --- | --- |
| `./nvr build --cli-only` | Build the CLI, worker, and model inspector without the web server/frontend |
| `./nvr cross-build` | Build and package Ubuntu 26.04 ARM64 on an x86_64 Linux host with Docker |
| `./nvr cli config/example.json` | Run without the dashboard; edit the example's model and source paths first |

Cross-build output is `.local/dist/rockchip-yolo-cpp-ubuntu26-arm64.tar.gz`.
Set `JOBS=8` to change build parallelism. Existing SDK installations can be
selected with `ROCKCHIP_SDK` and `MPP_ROOT`; see [tools/build.sh](tools/build.sh).

## Use the engine

Link the processing library from another CMake project:

```cmake
add_subdirectory(rockchip-yolo-cpp)
target_link_libraries(my_app PRIVATE nvr::engine)
```

Application targets are disabled by default when included this way.

| Interface | Purpose |
| --- | --- |
| [decoder.hpp](src/engine/decoder.hpp) | MPP decoding with frame and optional packet callbacks |
| [preprocess.hpp](src/engine/preprocess.hpp) | RGA/CPU preprocessing from borrowed NV12 image buffers |
| [detector_pool.hpp](src/engine/detector_pool.hpp) | Ordered inference across independent RKNN contexts |
| [task_model.hpp](src/engine/task_model.hpp) | Task tensor contracts, decoding, NMS, and portable result types |
| [postprocess.hpp](src/engine/postprocess.hpp) | Optimized detection postprocessing |

For portable postprocessing only, set `NVR_BUILD_ENGINE=OFF` and link
`nvr::postprocess`. It needs neither the Rockchip SDK nor FFmpeg.
For a standalone hardware library build, set `NVR_BUILD_APPS=OFF`.

Channel orchestration lives in `src/worker/`, HTTP/WebSocket APIs in `src/server/`,
and the dashboard in `web/`. Start with [src/cli.cpp](src/cli.cpp) to follow the pipeline.
C++ changes should follow `.clang-format` and be validated on the affected boards.

## Data and limitations

- Settings, uploads, recordings, and logs are stored in **`.local/state/`**. Preserve this directory when cleaning builds or upgrading.
- There is no automatic RTSP reconnect or recording retention. Monitor recording disk usage.
- Hardware access requires read/write permissions for the board's MPP, RGA, NPU/render, and DMA-heap devices.

When reporting a problem, include the board, OS, RKNN runtime/driver versions,
model export details, and relevant logs. Do not include camera credentials.
