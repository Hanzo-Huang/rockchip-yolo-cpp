import { useState } from "react";
import { api } from "../../lib/api";
import { bytes, seconds, type Asset, type Snapshot } from "../../lib/types";
import { ConfirmDelete } from "../../components/ConfirmDelete";
import { RenameDialog, ModelDetails } from "./AssetDialogs";
import { VideoPreview } from "./VideoPreview";
export function WarehousePage({
    kind,
    data,
    onUpload,
    refresh,
}: {
    kind: "video" | "model";
    data: Snapshot;
    onUpload: () => void;
    refresh: () => void;
}) {
    const [query, setQuery] = useState(""),
        [rename, setRename] = useState<Asset | null>(null),
        [deleting, setDeleting] = useState<Asset | null>(null),
        [preview, setPreview] = useState<Asset | null>(null);
    const assets = kind === "video" ? data.videos : data.models,
        visible = assets.filter((a) => a.name.toLowerCase().includes(query.toLowerCase()));
    const jobs = data.jobs
        .filter((j) => j.kind === kind && ["uploading", "validating", "rejected"].includes(j.state))
        .slice(0, 6);
    return (
        <>
            <div className="page-heading">
                <div>
                    <h1>
                        {kind === "video" ? "Video" : "Model"} warehouse{" "}
                        <span className="count">{assets.length}</span>
                    </h1>
                    <p>
                        {kind === "video"
                            ? "MP4 test footage"
                            : "metadata.yaml + .rknn · paired files or ZIP"}
                    </p>
                </div>
                <button className="primary" onClick={onUpload}>
                    <span aria-hidden="true">↑</span> Upload {kind}
                </button>
            </div>
            {jobs.length > 0 && (
                <div className="jobs" aria-live="polite">
                    {jobs.map((job) => (
                        <div
                            key={job.id}
                            className={`job ${job.state === "rejected" ? "error" : ""}`}
                        >
                            <strong>{job.name}</strong>
                            <span>
                                {job.state === "uploading"
                                    ? `${Math.round((job.received / job.total) * 100)}% uploaded`
                                    : job.state}
                            </span>
                            {job.error && <small>{job.error}</small>}
                        </div>
                    ))}
                </div>
            )}
            <div className="section-bar">
                <span>
                    {visible.length} {kind === "video" ? "videos" : "models"}
                </span>
                <input
                    className="search"
                    aria-label={`Search ${kind}s`}
                    placeholder={`Search ${kind}s…`}
                    value={query}
                    onChange={(e) => setQuery(e.target.value)}
                />
            </div>
            {!visible.length ? (
                <div className="empty-state">
                    <h2>{query ? "No matches" : `Your ${kind} warehouse is empty`}</h2>
                    <p>
                        {query
                            ? "Try a different search."
                            : `Upload your first ${kind} to get started.`}
                    </p>
                    {!query && (
                        <button className="primary" onClick={onUpload}>
                            Upload {kind}
                        </button>
                    )}
                </div>
            ) : (
                <div className="asset-grid">
                    {visible.map((asset) => (
                        <article key={asset.id} className="asset-card">
                            {kind === "video" ? (
                                <button
                                    className="asset-image"
                                    aria-label={`Preview ${asset.name}`}
                                    onClick={() => setPreview(asset)}
                                >
                                    {asset.info.thumbnail ? (
                                        <img
                                            src={`/api/v1/videos/${asset.id}/thumbnail`}
                                            alt=""
                                            loading="lazy"
                                        />
                                    ) : (
                                        <span className="thumbnail-placeholder">
                                            No preview available
                                        </span>
                                    )}
                                    <span className="play-overlay">▶</span>
                                    <span className="duration">{seconds(asset.info.duration)}</span>
                                </button>
                            ) : (
                                <button
                                    className="model-visual"
                                    aria-label={`Inspect ${asset.name}`}
                                    onClick={() => setPreview(asset)}
                                >
                                    <span className="model-chip">RKNN</span>
                                    <small>
                                        {asset.info.width} × {asset.info.height} ·{" "}
                                        {asset.info.names?.length ?? 0} classes
                                    </small>
                                </button>
                            )}
                            <div className="asset-body">
                                <div className="asset-title">
                                    <h3 title={asset.name}>{asset.name}</h3>
                                    <span
                                        className={`badge ${asset.status === "ready" ? "good" : "bad"}`}
                                    >
                                        {asset.status === "ready" ? "Ready" : "Incompatible"}
                                    </span>
                                </div>
                                <p className="muted">
                                    {kind === "video"
                                        ? `${asset.info.width} × ${asset.info.height} · ${String(asset.info.codec).toUpperCase()} · ${bytes(Number(asset.info.size_bytes ?? asset.info.size ?? 0))}`
                                        : `${String(asset.info.postprocessor ?? "Unrecognized layout")} · ${bytes(Number(asset.info.size_bytes ?? 0))}`}
                                </p>
                                <p className="asset-assignment">
                                    {asset.references.length
                                        ? `Assigned to ${asset.references.map((c) => c.name).join(", ")}`
                                        : "Not assigned to a channel"}
                                </p>
                                {asset.status !== "ready" && (
                                    <p className="error">{asset.info.reason}</p>
                                )}
                                <div className="asset-actions">
                                    <button onClick={() => setPreview(asset)}>
                                        {kind === "video" ? "Preview" : "Metadata"}
                                    </button>
                                    <button onClick={() => setRename(asset)}>Rename</button>
                                    <a
                                        href={`/api/v1/${kind}s/${asset.id}/download`}
                                        className="button"
                                    >
                                        Download
                                    </a>
                                    <button
                                        className="text-danger"
                                        onClick={() => setDeleting(asset)}
                                    >
                                        Delete
                                    </button>
                                </div>
                            </div>
                        </article>
                    ))}
                </div>
            )}
            {rename && (
                <RenameDialog asset={rename} onClose={() => setRename(null)} refresh={refresh} />
            )}
            {deleting && (
                <ConfirmDelete
                    name={deleting.name}
                    references={deleting.references}
                    onClose={() => setDeleting(null)}
                    onConfirm={async () => {
                        await api(`${kind}s/${deleting.id}`, "DELETE", {
                            confirm_name: deleting.name,
                        });
                        refresh();
                    }}
                />
            )}
            {preview &&
                (kind === "video" ? (
                    <VideoPreview asset={preview} onClose={() => setPreview(null)} />
                ) : (
                    <ModelDetails asset={preview} onClose={() => setPreview(null)} />
                ))}
        </>
    );
}
