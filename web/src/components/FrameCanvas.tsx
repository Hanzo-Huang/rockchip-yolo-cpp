import { useEffect, useRef, useState } from "react";
import { socketURL } from "../lib/api";
import type { InferenceResult as Metadata } from "../lib/inference";
import { drawResults } from "../lib/resultOverlay";
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
                        drawResults(context, meta, latest.current.labels);
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
            <canvas ref={canvas} aria-label="Hardware-decoded video with model result overlays" />
            {status && (
                <div className="frame-placeholder">
                    <span>{status}</span>
                </div>
            )}
            <span className="frame-tag">{pipeline}</span>
        </div>
    );
}
