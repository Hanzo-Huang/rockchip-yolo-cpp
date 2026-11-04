import { useEffect, useRef, useState } from "react";
import { socketURL } from "../lib/api";

interface Detection {
    run_id: string;
    epoch: number;
    pts_us: number;
    source_width: number;
    source_height: number;
    detections: { box: number[]; class_id: number; confidence: number }[];
}
interface VideoHeader {
    run_id: string;
    epoch: number;
    init: boolean;
    mime: string;
    origin_us: number;
}
export function NativeVideo({
    channel,
    run,
    epoch,
    labels,
    enabled,
    onFps,
    fileURL,
    paused = false,
    position = 0,
    sourceStart = 0,
}: {
    channel: string;
    run?: string;
    epoch?: number;
    labels?: string[];
    enabled: boolean;
    onFps?: (fps: number) => void;
    fileURL?: string;
    paused?: boolean;
    position?: number;
    sourceStart?: number;
}) {
    const video = useRef<HTMLVideoElement>(null),
        canvas = useRef<HTMLCanvasElement>(null);
    const latest = useRef({ run, epoch, labels, onFps, paused, position, sourceStart });
    latest.current = { run, epoch, labels, onFps, paused, position, sourceStart };
    const [status, setStatus] = useState("Waiting for video keyframe");
    useEffect(() => {
        latest.current.onFps?.(0);
        if (!enabled) {
            setStatus("Channel is not running");
            return;
        }
        const element = video.current!;
        if ((!fileURL && !window.MediaSource) || !element.requestVideoFrameCallback) {
            setStatus("This browser does not support continuous MediaSource video");
            return;
        }
        let disposed = false,
            socket: WebSocket | undefined,
            detector: WebSocket | undefined;
        let media: MediaSource | undefined,
            buffer: SourceBuffer | undefined,
            objectURL = "";
        let header: VideoHeader | undefined,
            queue: ArrayBuffer[] = [],
            history: Detection[] = [];
        let opened = false,
            playing = false,
            presented = 0,
            previousPresented = 0;
        let overlayKey = "";
        let presentedTime = 0;
        let frameCallback = 0,
            started = performance.now();
        let retry: ReturnType<typeof setTimeout> | undefined;
        const fail = (message: string) => {
            if (!disposed) setStatus(message);
        };
        const pump = () => {
            if (disposed || !buffer || buffer.updating) return;
            try {
                // Trim decoded media, not inference results, to keep memory bounded.
                if (
                    element.currentTime > 15 &&
                    buffer.buffered.length &&
                    buffer.buffered.start(0) < element.currentTime - 10
                ) {
                    buffer.remove(0, element.currentTime - 5);
                    return;
                }
                if (queue.length) {
                    buffer.appendBuffer(queue.shift()!);
                    return;
                }
            } catch (error) {
                fail(`Video buffer: ${String(error)}`);
            }
        };
        const playback = () => {
            if (!buffer?.buffered.length) return;
            const ranges = buffer.buffered,
                begin = ranges.start(0),
                end = ranges.end(ranges.length - 1);
            // Native video presentation handles cadence. Keep a modest jitter reserve;
            // never adjust playback speed according to AI throughput.
            if (!playing && end - begin >= 1) {
                element.currentTime = Math.max(begin, end - 1);
                playing = true;
                void element.play().catch(() => fail("Click the video to start playback"));
            } else if (playing && (element.currentTime < begin || end - element.currentTime > 3)) {
                element.currentTime = Math.max(begin, end - 1);
            }
        };
        const startMedia = (info: VideoHeader, data: ArrayBuffer) => {
            if (!MediaSource.isTypeSupported(info.mime)) {
                fail(`Browser cannot decode ${info.mime}`);
                return;
            }
            if (objectURL) URL.revokeObjectURL(objectURL);
            header = info;
            queue = [data];
            buffer = undefined;
            opened = false;
            playing = false;
            previousPresented = 0;
            presented = 0;
            started = performance.now();
            media = new MediaSource();
            objectURL = URL.createObjectURL(media);
            element.src = objectURL;
            const currentMedia = media;
            currentMedia.addEventListener(
                "sourceopen",
                () => {
                    if (disposed || media !== currentMedia || currentMedia.readyState !== "open")
                        return;
                    try {
                        buffer = currentMedia.addSourceBuffer(info.mime);
                        opened = true;
                        buffer.addEventListener("updateend", () => {
                            playback();
                            pump();
                        });
                        buffer.addEventListener("error", () =>
                            fail("Compressed video decode failed"),
                        );
                        pump();
                    } catch (error) {
                        fail(`Video initialization: ${String(error)}`);
                    }
                },
                { once: true },
            );
        };
        const draw = (_now: number, info: VideoFrameCallbackMetadata) => {
            if (disposed) return;
            presented += previousPresented ? info.presentedFrames - previousPresented : 1;
            previousPresented = info.presentedFrames;
            presentedTime = info.mediaTime;
            renderOverlay(info.mediaTime);
            setStatus("");
            frameCallback = element.requestVideoFrameCallback(draw);
        };
        const renderOverlay = (mediaTime: number) => {
            const target =
                mediaTime * 1e6 + (fileURL ? latest.current.sourceStart : (header?.origin_us ?? 0));
            let matching: Detection | undefined;
            for (let i = history.length - 1; i >= 0; i--) {
                const item = history[i];
                if (
                    item.pts_us <= target &&
                    item.run_id === header?.run_id &&
                    item.epoch === header?.epoch
                ) {
                    matching = item;
                    break;
                }
            }
            const surface = canvas.current,
                context = surface?.getContext("2d");
            if (surface && context && element.videoWidth) {
                // Overlay rasterization must match the displayed tile, not the full
                // camera sensor: clearing a 1080p/4K canvas per frame wastes CPU/GPU.
                const width = Math.min(
                    element.videoWidth,
                    Math.max(1, Math.round(element.clientWidth)),
                );
                const height = Math.max(
                    1,
                    Math.round((width * element.videoHeight) / element.videoWidth),
                );
                if (surface.width !== width || surface.height !== height) {
                    surface.width = width;
                    surface.height = height;
                }
                const valid = matching && target - matching.pts_us <= 200000;
                const key = `${header?.run_id}:${header?.epoch}:${valid ? matching?.pts_us : "empty"}:${width}:${height}`;
                if (key !== overlayKey) {
                    overlayKey = key;
                    context.clearRect(0, 0, surface.width, surface.height);
                    if (valid && matching) {
                        const sx = surface.width / matching.source_width,
                            sy = surface.height / matching.source_height;
                        context.font = "12px sans-serif";
                        context.lineWidth = 1.5;
                        for (const item of matching.detections) {
                            const [x1, y1, x2, y2] = item.box,
                                x = x1 * sx,
                                y = Math.max(17, y1 * sy);
                            const label = `${latest.current.labels?.[item.class_id] ?? `class ${item.class_id}`} ${(item.confidence * 100).toFixed(0)}%`;
                            context.strokeStyle = "#57e3bd";
                            context.strokeRect(x1 * sx, y1 * sy, (x2 - x1) * sx, (y2 - y1) * sy);
                            context.fillStyle = "#143f37";
                            context.fillRect(x, y - 17, context.measureText(label).width + 8, 19);
                            context.fillStyle = "#b4ffe9";
                            context.fillText(label, x + 4, y - 4);
                        }
                    }
                }
            }
        };
        const alignPaused = () => {
            if (!fileURL || !latest.current.paused || element.readyState < 1) return;
            for (let i = history.length - 1; i >= 0; i--) {
                const item = history[i];
                if (item.run_id !== header?.run_id || item.epoch !== header?.epoch) continue;
                const time = (item.pts_us - latest.current.sourceStart) / 1e6;
                // MP4 seeks round to a preceding frame; the worker's one-shot seek
                // analyses the first frame at/after the target. Show that exact frame.
                if (
                    Math.abs(element.currentTime - time) < 0.2 &&
                    Math.abs(presentedTime - time) > 0.001
                )
                    element.currentTime = time + 0.000001;
                break;
            }
        };
        const connect = () => {
            if (disposed || document.hidden) return;
            setStatus(fileURL ? "Loading original video" : "Waiting for video keyframe");
            if (fileURL) {
                header = {
                    run_id: latest.current.run ?? "",
                    epoch: latest.current.epoch ?? 0,
                    origin_us: latest.current.sourceStart,
                    init: true,
                    mime: "",
                };
                element.src = fileURL;
            } else {
                socket = new WebSocket(
                    socketURL(`/ws/video?channel=${encodeURIComponent(channel)}`),
                );
                socket.binaryType = "arraybuffer";
                socket.onmessage = (event) => {
                    if (disposed) return;
                    try {
                        if (typeof event.data === "string") {
                            fail(JSON.parse(event.data).error);
                            return;
                        }
                        const packet = event.data as ArrayBuffer,
                            length = new DataView(packet).getUint32(0);
                        const info: VideoHeader = JSON.parse(
                            new TextDecoder().decode(new Uint8Array(packet, 4, length)),
                        );
                        if (
                            (latest.current.run && info.run_id !== latest.current.run) ||
                            (latest.current.epoch && info.epoch < latest.current.epoch)
                        )
                            return;
                        const data = packet.slice(4 + length);
                        if (info.init) startMedia(info, data);
                        else {
                            queue.push(data);
                            if (opened) pump();
                        }
                    } catch (error) {
                        fail(`Video transport: ${String(error)}`);
                    }
                };
                socket.onclose = () => {
                    if (!disposed && !document.hidden) retry = setTimeout(connect, 1500);
                };
            }
            detector?.close();
            detector = new WebSocket(
                socketURL(`/ws/detections?channel=${encodeURIComponent(channel)}`),
            );
            detector.onmessage = (event) => {
                try {
                    history.push(JSON.parse(event.data));
                    if (history.length > 240) history.shift();
                    if (fileURL && latest.current.paused) {
                        alignPaused();
                        renderOverlay(presentedTime);
                    }
                } catch {
                    /* Ignore invalid metadata. */
                }
            };
        };
        const visibility = () => {
            clearTimeout(retry);
            if (document.hidden) {
                socket?.close();
                detector?.close();
                element.pause();
                presented = 0;
                latest.current.onFps?.(0);
            } else connect();
        };
        const ready = () => {
            if (!fileURL || disposed) return;
            element.currentTime = latest.current.position;
            alignPaused();
            if (!latest.current.paused)
                void element.play().catch(() => fail("Click the video to start playback"));
        };
        const fileError = () => {
            if (fileURL) fail("This browser cannot play the uploaded video codec");
        };
        element.addEventListener("loadedmetadata", ready);
        element.addEventListener("error", fileError);
        const timer = setInterval(() => {
            const now = performance.now();
            latest.current.onFps?.(
                document.hidden || (fileURL && latest.current.paused)
                    ? 0
                    : (presented * 1000) / (now - started),
            );
            presented = 0;
            started = now;
        }, 1000);
        frameCallback = element.requestVideoFrameCallback(draw);
        connect();
        document.addEventListener("visibilitychange", visibility);
        return () => {
            disposed = true;
            clearTimeout(retry);
            clearInterval(timer);
            socket?.close();
            detector?.close();
            element.cancelVideoFrameCallback(frameCallback);
            element.pause();
            element.removeAttribute("src");
            element.load();
            if (objectURL) URL.revokeObjectURL(objectURL);
            document.removeEventListener("visibilitychange", visibility);
            element.removeEventListener("loadedmetadata", ready);
            element.removeEventListener("error", fileError);
        };
    }, [channel, enabled, run, epoch, fileURL]);
    useEffect(() => {
        if (!fileURL || !enabled || !video.current) return;
        if (paused) {
            video.current.pause();
            latest.current.onFps?.(0);
        } else if (video.current.readyState >= 1) void video.current.play().catch(() => {});
    }, [fileURL, enabled, paused]);
    return (
        <div className="frame native-stream">
            <video
                ref={video}
                muted
                playsInline
                aria-label="Continuous original video"
                onClick={() => void video.current?.play()}
            />
            <canvas ref={canvas} aria-label="Timestamp-matched detection overlays" />
            {status && (
                <div className="frame-placeholder">
                    <span>{status}</span>
                </div>
            )}
            <span className="frame-tag">
                {fileURL ? "Original file" : "Original stream"} · browser decode
            </span>
        </div>
    );
}
