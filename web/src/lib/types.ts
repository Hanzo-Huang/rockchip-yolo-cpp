import type { ModelTask } from "./inference";

export interface Runtime {
    state: string;
    run_id?: string;
    epoch?: number;
    position?: number;
    duration?: number;
    source_start_us?: number;
    decode_fps?: number;
    inference_fps?: number;
    preprocess_ms?: number;
    inference_ms?: number;
    objects?: number;
    task?: ModelTask;
    top_class?: number;
    top_confidence?: number;
    queue_drops?: number;
    error?: string;
    preview_error?: string;
}
export interface Asset {
    id: string;
    name: string;
    kind: "video" | "model";
    status: string;
    created_at: string;
    references: { id: string; name: string }[];
    info: {
        task?: ModelTask;
        width?: number;
        height?: number;
        duration?: number;
        codec?: string;
        size?: number;
        names?: string[];
        imgsz?: number[];
        compatible?: boolean;
        reason?: string;
        compatibility_error?: string;
        thumbnail?: boolean;
        accuracy_verified?: boolean;
        [key: string]: unknown;
    };
}
export interface ChannelConfig {
    source_type: "file" | "rtsp" | "camera";
    rtsp_display?: string;
    rtsp_url?: string;
    camera_id?: string;
    preprocess: "rga" | "cpu";
    detect_every: number;
    confidence: number;
    nms_iou: number;
    recording: boolean;
}
export interface Channel {
    id: string;
    name: string;
    video_id: string | null;
    model_id: string;
    config: ChannelConfig;
    runtime: Runtime;
}
export interface Job {
    id: string;
    kind: string;
    name: string;
    state: string;
    received: number;
    total: number;
    error?: string;
    asset_status?: string;
}
export interface Snapshot {
    channels: Channel[];
    videos: Asset[];
    models: Asset[];
    jobs: Job[];
    system: {
        cpu_percent: number | null;
        memory_total: number;
        memory_used: number;
        storage_free: number;
        storage_total: number;
        temperature_c: number | null;
        npu_load: string | null;
    };
}
export interface Camera {
    id: string;
    name: string;
    supported: boolean;
    formats: string[];
    reason: string;
}
export const active = (state: string) =>
    ["starting", "running", "paused", "analytics_paused", "seeking", "stopping"].includes(state);
export const seconds = (value = 0) =>
    `${Math.floor(value / 60)}:${Math.floor(value % 60)
        .toString()
        .padStart(2, "0")}`;
export const bytes = (value = 0) =>
    value >= 1024 ** 3
        ? `${(value / 1024 ** 3).toFixed(1)} GB`
        : `${(value / 1024 ** 2).toFixed(1)} MB`;
