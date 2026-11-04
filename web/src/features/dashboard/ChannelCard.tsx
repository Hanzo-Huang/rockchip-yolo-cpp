import { useState } from "react";
import { api } from "../../lib/api";
import { active, seconds, type Asset, type Channel } from "../../lib/types";
import { NativeVideo } from "../../components/NativeVideo";
export function ChannelCard({
    channel,
    model,
    video,
    onEdit,
    onDelete,
    onFocus,
    onRefresh,
}: {
    channel: Channel;
    model?: Asset;
    video?: Asset;
    focused?: boolean;
    onEdit: () => void;
    onDelete: () => void;
    onFocus: () => void;
    onRefresh: () => void;
}) {
    const runtime = channel.runtime,
        running = active(runtime.state),
        paused = ["paused", "analytics_paused"].includes(runtime.state);
    const [busy, setBusy] = useState(false),
        [error, setError] = useState(""),
        [videoFps, setVideoFps] = useState(0),
        [seek, setSeek] = useState<number | null>(null);
    async function action(name: string, extra = {}) {
        setBusy(true);
        setError("");
        try {
            await api(`channels/${channel.id}/actions`, "POST", {
                action: name,
                ...extra,
            });
            onRefresh();
        } catch (e) {
            setError(e instanceof Error ? e.message : "Command failed");
        } finally {
            setBusy(false);
        }
    }
    const source =
        channel.config.source_type === "file"
            ? (video?.name ?? "Video file")
            : channel.config.source_type === "rtsp"
              ? channel.config.rtsp_display
              : channel.config.camera_id;
    return (
        <article className="channel-card">
            <div className="channel-visual">
                <header className="card-head">
                    <div>
                        <h3>{channel.name}</h3>
                    </div>
                    <span className={`badge ${runtime.state === "running" ? "good" : ""}`}>
                        {runtime.state.replaceAll("_", " ")}
                    </span>
                </header>
                <button
                    className="preview-button"
                    aria-label={`Focus ${channel.name}`}
                    onClick={onFocus}
                >
                    <NativeVideo
                        channel={channel.id}
                        run={runtime.run_id}
                        epoch={runtime.epoch}
                        labels={model?.info.names}
                        enabled={
                            running ||
                            (channel.config.source_type === "file" && runtime.state === "ended")
                        }
                        onFps={setVideoFps}
                        fileURL={
                            channel.config.source_type === "file" && channel.video_id
                                ? `/api/v1/videos/${channel.video_id}/content`
                                : undefined
                        }
                        paused={runtime.state === "paused" || runtime.state === "seeking"}
                        position={runtime.position}
                        sourceStart={runtime.source_start_us}
                    />
                </button>
            </div>
            <div className="channel-body">
                <div className="channel-metrics">
                    <div title="Video frames presented by this browser per second, measured over the last second">
                        <strong data-video-fps>{videoFps.toFixed(1)}</strong>
                        <small>video fps</small>
                    </div>
                    <div>
                        <strong>{(runtime.decode_fps ?? 0).toFixed(1)}</strong>
                        <small>decode fps</small>
                    </div>
                    <div>
                        <strong>{(runtime.inference_fps ?? 0).toFixed(1)}</strong>
                        <small>inference fps</small>
                    </div>
                    <div>
                        <strong>
                            {(runtime.preprocess_ms ?? 0).toFixed(2)}
                            <em>ms</em>
                        </strong>
                        <small>preprocess</small>
                    </div>
                    <div>
                        <strong>{runtime.objects ?? 0}</strong>
                        <small>objects</small>
                    </div>
                </div>
                {channel.config.source_type === "file" && (
                    <div className="timeline">
                        <input
                            aria-label={`Seek ${channel.name}`}
                            type="range"
                            min={0}
                            max={Math.max(0, (runtime.duration ?? 0) - 0.05)}
                            step={0.05}
                            value={seek ?? runtime.position ?? 0}
                            disabled={
                                !running || busy || !runtime.duration || runtime.state === "seeking"
                            }
                            onChange={(e) => setSeek(Number(e.target.value))}
                            onPointerUp={(e) => {
                                void action("seek", { seconds: Number(e.currentTarget.value) });
                                setSeek(null);
                            }}
                            onKeyUp={(e) => {
                                if (
                                    [
                                        "ArrowLeft",
                                        "ArrowRight",
                                        "Home",
                                        "End",
                                        "PageUp",
                                        "PageDown",
                                    ].includes(e.key)
                                ) {
                                    void action("seek", {
                                        seconds: Number(e.currentTarget.value),
                                    });
                                    setSeek(null);
                                }
                            }}
                        />
                        <span>
                            {seconds(seek ?? runtime.position)} / {seconds(runtime.duration)}
                        </span>
                    </div>
                )}
                <div className="card-controls">
                    <button
                        className={running ? "" : "primary"}
                        disabled={
                            busy || ["starting", "stopping", "seeking"].includes(runtime.state)
                        }
                        onClick={() =>
                            void action(running ? (paused ? "resume" : "pause") : "start")
                        }
                    >
                        {running ? (paused ? "▶ Resume" : "Ⅱ Pause") : "▶ Start"}
                    </button>
                    <button disabled={!running || busy} onClick={() => void action("stop")}>
                        ■ Stop
                    </button>
                    <button onClick={onEdit} disabled={busy}>
                        Edit
                    </button>
                    <button
                        className="text-danger"
                        onClick={onDelete}
                        disabled={running || busy}
                        title={running ? "Stop before deleting" : "Delete channel"}
                    >
                        Delete
                    </button>
                </div>
                <div className="card-foot">
                    <span title={`${source} · ${model?.name ?? "No model"}`}>
                        {source} · {model?.name ?? "No model"}
                    </span>
                    <span>
                        {channel.config.recording ? "● Recording enabled" : "Recording off"}
                    </span>
                </div>
                {(error || runtime.error || runtime.preview_error) && (
                    <p className="error" role="alert">
                        {error || runtime.error || runtime.preview_error}
                    </p>
                )}
                {runtime.state === "analytics_paused" && (
                    <p className="muted">Analytics paused; live preview and recording continue.</p>
                )}
            </div>
        </article>
    );
}
