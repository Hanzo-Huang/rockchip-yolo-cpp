import { useEffect, useRef, useState } from "react";
import { socketURL } from "../lib/api";
interface Metadata {
    run_id: string;
    epoch: number;
    source_width: number;
    source_height: number;
    detections: { box: number[]; class_id: number; confidence: number }[];
}
export function FrameCanvas({
    channel,
    run,
    epoch,
    labels,
    focused = false,
    enabled = true,
    pipeline = "MPP → RGA → JPEG",
    onFps,
}: {
    channel: string;
    run?: string;
    epoch?: number;
    labels?: string[];
    focused?: boolean;
    enabled?: boolean;
    pipeline?: string;
    onFps?: (fps: number) => void;
}) {
    const canvas = useRef<HTMLCanvasElement>(null),
        drawnFrames = useRef(0),
        latest = useRef({ run, epoch, labels, onFps });
    latest.current = { run, epoch, labels, onFps };
    const [status, setStatus] = useState("Waiting for preview");
    useEffect(() => {
        // Count completed canvas draws, not received packets or decoder frames.
        // Only publish once per second; per-frame bookkeeping never renders React.
        drawnFrames.current = 0;
        latest.current.onFps?.(0);
        if (!enabled) return;
        let started = performance.now();
        const timer = setInterval(() => {
            const now = performance.now();
            latest.current.onFps?.(
                document.hidden ? 0 : (drawnFrames.current * 1000) / (now - started),
            );
            drawnFrames.current = 0;
            started = now;
        }, 1000);
        return () => clearInterval(timer);
    }, [channel, run, epoch, enabled, focused]);
    useEffect(() => {
        if (!enabled) {
            setStatus("Channel is not running");
            return;
        }
        let disposed = false,
            socket: WebSocket | undefined,
            retry: ReturnType<typeof setTimeout>;
        const connect = () => {
            if (disposed || document.hidden) return;
            socket = new WebSocket(socketURL(`/ws/preview?channel=${encodeURIComponent(channel)}`));
            socket.binaryType = "arraybuffer";
            socket.onmessage = async (event) => {
                const current = socket;
                try {
                    const data = event.data as ArrayBuffer,
                        length = new DataView(data).getUint32(0);
                    const meta: Metadata = JSON.parse(
                        new TextDecoder().decode(new Uint8Array(data, 4, length)),
                    );
                    if (
                        (latest.current.run && meta.run_id !== latest.current.run) ||
                        (latest.current.epoch && meta.epoch < latest.current.epoch)
                    )
                        return;
                    const bitmap = await createImageBitmap(
                        new Blob([data.slice(4 + length)], { type: "image/jpeg" }),
                    );
                    try {
                        if (
                            disposed ||
                            document.hidden ||
                            !canvas.current ||
                            (latest.current.run && meta.run_id !== latest.current.run) ||
                            (latest.current.epoch && meta.epoch < latest.current.epoch)
                        )
                            return;
                        const element = canvas.current,
                            context = element.getContext("2d");
                        if (!context) return;
                        element.width = bitmap.width;
                        element.height = bitmap.height;
                        context.drawImage(bitmap, 0, 0);
                        const sx = bitmap.width / meta.source_width,
                            sy = bitmap.height / meta.source_height;
                        context.font = "11px ui-monospace, monospace";
                        context.lineWidth = 1.5;
                        for (const item of meta.detections) {
                            const [x1, y1, x2, y2] = item.box;
                            const label = `${latest.current.labels?.[item.class_id] ?? `class ${item.class_id}`} ${(item.confidence * 100).toFixed(0)}%`;
                            const x = Math.max(0, x1 * sx),
                                y = Math.max(13, y1 * sy);
                            context.strokeStyle = "#57e3bd";
                            context.strokeRect(x1 * sx, y1 * sy, (x2 - x1) * sx, (y2 - y1) * sy);
                            context.fillStyle = "#143f37";
                            context.fillRect(x, y - 13, context.measureText(label).width + 8, 14);
                            context.fillStyle = "#b4ffe9";
                            context.fillText(label, x + 4, y - 3);
                        }
                        drawnFrames.current += 1;
                        setStatus("");
                    } finally {
                        bitmap.close();
                    }
                } catch {
                    if (!disposed) setStatus("Preview decode failed");
                } finally {
                    if (current?.readyState === WebSocket.OPEN) current.send("ack");
                }
            };
            socket.onclose = () => {
                if (!disposed && !document.hidden) retry = setTimeout(connect, 1500);
            };
        };
        const visibility = () => {
            clearTimeout(retry);
            if (document.hidden) socket?.close();
            else connect();
        };
        connect();
        document.addEventListener("visibilitychange", visibility);
        return () => {
            disposed = true;
            clearTimeout(retry);
            socket?.close();
            document.removeEventListener("visibilitychange", visibility);
        };
    }, [channel, focused, enabled]);
    return (
        <div className="frame">
            <canvas ref={canvas} aria-label="Hardware-decoded video with detection overlays" />
            {status && (
                <div className="frame-placeholder">
                    <span>{status}</span>
                </div>
            )}
            <span className="frame-tag">{pipeline}</span>
        </div>
    );
}
