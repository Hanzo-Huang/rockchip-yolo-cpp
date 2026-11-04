import { useEffect, useState } from "react";
import { api } from "../../lib/api";
import { seconds, type Asset, type Runtime } from "../../lib/types";
import { Dialog } from "../../components/Dialog";
import { FrameCanvas } from "../../components/FrameCanvas";
export function VideoPreview({ asset, onClose }: { asset: Asset; onClose: () => void }) {
    const [hardware, setHardware] = useState(false),
        [runtime, setRuntime] = useState<Runtime>({ state: "starting" }),
        [error, setError] = useState("");
    const id = `preview-${asset.id}`;
    useEffect(() => {
        if (!hardware) return;
        let disposed = false,
            started = false,
            timer: ReturnType<typeof setTimeout>;
        const poll = async () => {
            try {
                const state = await api<Runtime>(`previews/${id}`);
                if (!disposed) setRuntime(state);
            } catch (e) {
                if (!disposed) setError(String(e));
            }
            if (!disposed) timer = setTimeout(poll, 500);
        };
        void api(`videos/${asset.id}/preview`, "POST", {})
            .then(() => {
                started = true;
                if (disposed) void api(`previews/${id}`, "DELETE").catch(() => {});
                else void poll();
            })
            .catch((e) => {
                if (!disposed) setError(String(e));
            });
        return () => {
            disposed = true;
            clearTimeout(timer);
            if (started) void api(`previews/${id}`, "DELETE").catch(() => {});
        };
    }, [hardware, asset.id, id]);
    async function action(action: string, extra = {}) {
        try {
            await api(`previews/${id}/actions`, "POST", { action, ...extra });
        } catch (e) {
            setError(String(e));
        }
    }
    return (
        <Dialog title={`Preview · ${asset.name}`} wide onClose={onClose}>
            {!hardware ? (
                <>
                    <video
                        className="native-video"
                        controls
                        autoPlay
                        src={`/api/v1/videos/${asset.id}/content`}
                        onError={() =>
                            setError(
                                "Your browser cannot play this codec. Try board hardware preview.",
                            )
                        }
                    />
                    <div className="actions">
                        <span className="muted">Original file · browser playback</span>
                        <button
                            disabled={asset.status !== "ready"}
                            onClick={() => {
                                setError("");
                                setHardware(true);
                            }}
                        >
                            Use board hardware preview
                        </button>
                    </div>
                </>
            ) : (
                <>
                    <FrameCanvas channel={id} run={runtime.run_id} epoch={runtime.epoch} focused />
                    <div className="timeline">
                        <input
                            aria-label="Seek hardware preview"
                            type="range"
                            min={0}
                            max={Math.max(0, (runtime.duration ?? asset.info.duration ?? 0) - 0.05)}
                            step={0.1}
                            value={runtime.position ?? 0}
                            onChange={(e) =>
                                setRuntime({ ...runtime, position: Number(e.target.value) })
                            }
                            onPointerUp={(e) =>
                                void action("seek", { seconds: Number(e.currentTarget.value) })
                            }
                            onKeyUp={(e) => {
                                if (e.key.startsWith("Arrow"))
                                    void action("seek", { seconds: Number(e.currentTarget.value) });
                            }}
                        />
                        <span>
                            {seconds(runtime.position)} / {seconds(runtime.duration)}
                        </span>
                    </div>
                    <div className="actions">
                        <span className="badge">{runtime.state}</span>
                        <button
                            onClick={() =>
                                void action(runtime.state === "paused" ? "resume" : "pause")
                            }
                        >
                            {runtime.state === "paused" ? "Resume" : "Pause"}
                        </button>
                        <button onClick={() => setHardware(false)}>Back to original</button>
                    </div>
                </>
            )}
            {(error || runtime.error || runtime.preview_error) && (
                <p className="error" role="alert">
                    {error || runtime.error || runtime.preview_error}
                </p>
            )}
        </Dialog>
    );
}
