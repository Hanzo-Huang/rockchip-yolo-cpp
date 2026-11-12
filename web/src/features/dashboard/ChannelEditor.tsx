import { useEffect, useState } from "react";
import { api } from "../../lib/api";
import { active, type Camera, type Channel, type Snapshot } from "../../lib/types";
import { Dialog } from "../../components/Dialog";
import { ClassPicker } from "./ClassPicker";
export function ChannelEditor({
    channel,
    data,
    onClose,
    onSaved,
    onUpload,
}: {
    channel?: Channel;
    data: Snapshot;
    onClose: () => void;
    onSaved: () => void;
    onUpload: () => void;
}) {
    const [name, setName] = useState(channel?.name ?? "");
    const [source, setSource] = useState(channel?.config.source_type ?? "file");
    const [video, setVideo] = useState(
        channel?.video_id ?? data.videos.find((v) => v.status === "ready")?.id ?? "",
    );
    const [model, setModel] = useState(
        channel?.model_id ?? data.models.find((m) => m.status === "ready")?.id ?? "",
    );
    const [url, setURL] = useState(channel?.config.rtsp_url ?? ""),
        [camera, setCamera] = useState(channel?.config.camera_id ?? "");
    const [preprocess, setPreprocess] = useState(channel?.config.preprocess ?? "rga");
    const [every, setEvery] = useState(channel?.config.detect_every ?? 1),
        [confidence, setConfidence] = useState(channel?.config.confidence ?? 0.35);
    const [recording, setRecording] = useState(channel?.config.recording ?? false),
        [restart, setRestart] = useState(true);
    const [classIds, setClassIds] = useState<number[] | null>(channel?.config.class_ids ?? null);
    const [cameras, setCameras] = useState<Camera[]>([]),
        [busy, setBusy] = useState(false),
        [error, setError] = useState("");
    useEffect(() => {
        const controller = new AbortController();
        void api<Camera[]>("cameras", "GET", undefined, controller.signal)
            .then(setCameras)
            .catch((e) => {
                if (!controller.signal.aborted) setError(String(e));
            });
        return () => controller.abort();
    }, []);
    const currentChannel = data.channels.find((item) => item.id === channel?.id) ?? channel;
    const isActive = !!currentChannel && active(currentChannel.runtime.state);
    const modelInfo = data.models.find((asset) => asset.id === model)?.info;
    const labels = modelInfo?.names;
    const canFilterClasses = ["detect", "segment", "pose", "obb"].includes(modelInfo?.task ?? "detect");
    async function save() {
        setBusy(true);
        setError("");
        try {
            let restartAfterSave = false;
            if (channel) {
                const current = await api<Channel>(`channels/${channel.id}`);
                if (active(current.runtime.state)) {
                    restartAfterSave = restart && current.runtime.state !== "stopping";
                    try {
                        await api(`channels/${channel.id}/actions`, "POST", { action: "stop" });
                    } catch (stopError) {
                        // EOF can arrive between the state check and the stop request.
                        const latest = await api<Channel>(`channels/${channel.id}`);
                        if (active(latest.runtime.state)) throw stopError;
                        restartAfterSave = false;
                    }
                }
                let stopped = false;
                for (let i = 0; i < 60; i++) {
                    const current = await api<Channel>(`channels/${channel.id}`);
                    if (!active(current.runtime.state)) {
                        stopped = true;
                        break;
                    }
                    await new Promise((resolve) => setTimeout(resolve, 200));
                }
                if (!stopped)
                    throw new Error(
                        "Channel is still stopping; wait before changing its configuration.",
                    );
            }
            const body = {
                name,
                source_type: source,
                video_id: video,
                model_id: model,
                rtsp_url: url,
                camera_id: camera,
                preprocess,
                detect_every: every,
                confidence,
                nms_iou: channel?.config.nms_iou ?? 0.45,
                recording,
                class_ids: canFilterClasses ? classIds : null,
            };
            const saved = await api<Channel>(
                channel ? `channels/${channel.id}` : "channels",
                channel ? "PATCH" : "POST",
                body,
            );
            if (restartAfterSave)
                await api(`channels/${saved.id}/actions`, "POST", { action: "start" });
            onSaved();
            onClose();
        } catch (e) {
            setError(e instanceof Error ? e.message : "Could not save channel");
            setBusy(false);
            onSaved();
        }
    }
    return (
        <Dialog title={channel ? "Edit channel" : "Add channel"} onClose={onClose}>
            <form
                onSubmit={(e) => {
                    e.preventDefault();
                    void save();
                }}
            >
                <label>
                    Channel name
                    <input
                        required
                        maxLength={120}
                        placeholder="New channel"
                        value={name}
                        onChange={(e) => setName(e.target.value)}
                        disabled={busy}
                    />
                </label>
                <div className="form-grid">
                    <label>
                        Source
                        <select
                            aria-label="Source"
                            value={source}
                            onChange={(e) => setSource(e.target.value as typeof source)}
                            disabled={busy}
                        >
                            <option value="file">Video warehouse</option>
                            <option value="rtsp">RTSP stream</option>
                            <option value="camera">Detected camera</option>
                        </select>
                    </label>
                    <label>
                        Model
                        <select
                            aria-label="Model"
                            required
                            value={model}
                            onChange={(e) => {
                                setModel(e.target.value);
                                setClassIds(null);
                            }}
                            disabled={busy}
                        >
                            <option value="">Select a model</option>
                            {data.models.map((m) => (
                                <option key={m.id} value={m.id} disabled={m.status !== "ready"}>
                                    {m.name}
                                    {m.status !== "ready" ? " · incompatible" : ""}
                                </option>
                            ))}
                        </select>
                    </label>
                </div>
                {source === "file" && (
                    <label>
                        Video file
                        <div className="input-row">
                            <select
                                required
                                value={video}
                                onChange={(e) => setVideo(e.target.value)}
                                disabled={busy}
                            >
                                <option value="">Select a video</option>
                                {data.videos.map((v) => (
                                    <option key={v.id} value={v.id} disabled={v.status !== "ready"}>
                                        {v.name}
                                    </option>
                                ))}
                            </select>
                            <button type="button" onClick={onUpload} disabled={busy}>
                                Upload
                            </button>
                        </div>
                    </label>
                )}
                {source === "rtsp" && (
                    <label>
                        RTSP URL
                        <input
                            type="text"
                            autoComplete="off"
                            spellCheck={false}
                            value={url}
                            required={!channel || channel.config.source_type !== "rtsp"}
                            placeholder={channel?.config.rtsp_display || "rtsp://camera/stream"}
                            onChange={(e) => setURL(e.target.value)}
                            disabled={busy}
                        />
                        <small>
                            {channel?.config.source_type === "rtsp" && !channel.config.rtsp_url
                                ? "Saved credentials are hidden. Leave blank to keep the current URL, or enter a replacement."
                                : "Edit the saved stream URL directly."}
                        </small>
                    </label>
                )}
                {source === "camera" && (
                    <label>
                        Detected camera
                        <select
                            required
                            value={camera}
                            onChange={(e) => setCamera(e.target.value)}
                            disabled={busy}
                        >
                            <option value="">
                                {cameras.length ? "Select camera" : "No capture cameras detected"}
                            </option>
                            {cameras.map((c) => (
                                <option key={c.id} value={c.id} disabled={!c.supported}>
                                    {c.name} · {c.formats.join(", ")}
                                    {!c.supported ? " (unsupported)" : ""}
                                </option>
                            ))}
                        </select>
                        <small>
                            H.264 V4L2 capture only. MJPEG, raw and CSI capture are not enabled.
                        </small>
                    </label>
                )}
                {canFilterClasses && labels?.length ? (
                    <ClassPicker
                        key={model}
                        names={labels}
                        selected={classIds}
                        onChange={setClassIds}
                        disabled={busy}
                    />
                ) : null}
                <details>
                    <summary>Processing settings</summary>
                    <div className="form-grid">
                        <label>
                            Preprocessing
                            <select
                                value={preprocess}
                                onChange={(e) => setPreprocess(e.target.value as typeof preprocess)}
                            >
                                <option value="rga">RGA · hardware DMA path</option>
                                <option value="cpu">CPU · comparison baseline</option>
                            </select>
                        </label>
                        <label>
                            Infer every N frames
                            <input
                                type="number"
                                min={1}
                                max={30}
                                value={every}
                                onChange={(e) => setEvery(Number(e.target.value))}
                            />
                        </label>
                        <label>
                            Confidence
                            <input
                                type="number"
                                min={0.01}
                                max={0.99}
                                step={0.01}
                                value={confidence}
                                onChange={(e) => setConfidence(Number(e.target.value))}
                            />
                        </label>
                    </div>
                    <label className="check">
                        <input
                            type="checkbox"
                            checked={recording}
                            onChange={(e) => setRecording(e.target.checked)}
                        />
                        Record compressed source without re-encoding
                    </label>
                </details>
                {isActive && (
                    <div className="notice">
                        If still active when you save, this channel will stop before changing
                        configuration.
                        <label className="check">
                            <input
                                type="checkbox"
                                checked={restart}
                                onChange={(e) => setRestart(e.target.checked)}
                            />
                            Restart this channel after saving
                        </label>
                    </div>
                )}
                {error && (
                    <p className="error" role="alert">
                        {error}
                    </p>
                )}
                <footer className="actions">
                    <button type="button" onClick={onClose} disabled={busy}>
                        Cancel
                    </button>
                    <button className="primary" disabled={busy}>
                        {busy ? "Saving…" : "Save channel"}
                    </button>
                </footer>
            </form>
        </Dialog>
    );
}
