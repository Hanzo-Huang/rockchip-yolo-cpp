export type ModelTask = "detect" | "segment" | "pose" | "obb" | "classify" | "semantic" | "depth";

export interface ResultRaster {
    width: number;
    height: number;
    box: number[];
    runs: number[];
    offset: number;
    scale: number;
}

export interface TaskDetection {
    box: number[];
    class_id: number;
    confidence: number;
    keypoints?: number[][];
    polygon?: number[][];
    mask?: ResultRaster;
}

export interface InferenceResult {
    run_id: string;
    epoch: number;
    pts_us: number;
    source_width: number;
    source_height: number;
    inference_ms?: number;
    task?: ModelTask;
    detections: TaskDetection[];
    classification?: { class_id: number; confidence: number }[];
    dense?: ResultRaster;
}
