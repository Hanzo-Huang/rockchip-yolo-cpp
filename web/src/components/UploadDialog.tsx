import { useEffect, useRef, useState } from "react";
import { upload } from "../lib/api";
import { Dialog } from "./Dialog";
export function UploadDialog({
    kind,
    onClose,
    onComplete,
}: {
    kind: "video" | "model";
    onClose: () => void;
    onComplete: () => void;
}) {
    const [name, setName] = useState(""),
        [files, setFiles] = useState<File[]>([]),
        [mode, setMode] = useState<"zip" | "pair">("zip"),
        [rknn, setRknn] = useState<File | null>(null),
        [yaml, setYaml] = useState<File | null>(null),
        [busy, setBusy] = useState(false),
        [progress, setProgress] = useState(0),
        [error, setError] = useState("");
    const controller = useRef<AbortController | null>(null);
    const ready = kind === "video" || mode === "zip" ? files.length === 1 : !!rknn && !!yaml;
    useEffect(() => () => controller.current?.abort(), []);
    return (
        <Dialog title={kind === "video" ? "Upload video" : "Upload model"} onClose={onClose}>
            <form
                onSubmit={async (e) => {
                    e.preventDefault();
                    if (!ready) return;
                    setBusy(true);
                    setError("");
                    controller.current = new AbortController();
                    try {
                        const selected =
                            kind === "model" && mode === "pair" && rknn && yaml
                                ? [
                                      rknn,
                                      new File([yaml], "metadata.yaml", {
                                          type: yaml.type,
                                          lastModified: yaml.lastModified,
                                      }),
                                  ]
                                : files;
                        await upload(kind, name, selected, setProgress, controller.current.signal);
                        onComplete();
                        onClose();
                    } catch (e) {
                        setError(e instanceof Error ? e.message : "Upload failed");
                        setBusy(false);
                    }
                }}
            >
                <p className="muted">
                    {kind === "video"
                        ? "One MP4. H.264 / H.265 8-bit video uses MPP decoding."
                        : "Upload one ZIP, or choose the RKNN model and YAML metadata separately."}
                </p>
                {kind === "model" && (
                    <div className="upload-mode" role="group" aria-label="Model upload format">
                        <label className="check">
                            <input
                                type="radio"
                                name="model-format"
                                checked={mode === "zip"}
                                disabled={busy}
                                onChange={() => setMode("zip")}
                            />{" "}
                            ZIP bundle
                        </label>
                        <label className="check">
                            <input
                                type="radio"
                                name="model-format"
                                checked={mode === "pair"}
                                disabled={busy}
                                onChange={() => setMode("pair")}
                            />{" "}
                            Separate files
                        </label>
                    </div>
                )}
                {kind === "model" && (
                    <div hidden={mode !== "pair"}>
                        <label className="file-picker">
                            RKNN model
                            <input
                                aria-label="RKNN model file"
                                type="file"
                                accept=".rknn"
                                disabled={busy}
                                onChange={(e) => {
                                    const file = e.target.files?.[0] ?? null;
                                    setRknn(file);
                                    if (!name && file) setName(file.name.replace(/\.[^.]+$/, ""));
                                }}
                            />
                        </label>
                        <label className="file-picker">
                            YAML metadata
                            <input
                                aria-label="YAML metadata file"
                                type="file"
                                accept=".yaml,.yml"
                                disabled={busy}
                                onChange={(e) => setYaml(e.target.files?.[0] ?? null)}
                            />
                            <small>Stored as metadata.yaml with the selected RKNN model.</small>
                        </label>
                    </div>
                )}
                <div hidden={kind === "model" && mode === "pair"}>
                    <label className="file-picker">
                        {kind === "video" ? "Video file" : "ZIP bundle"}
                        <input
                            aria-label="Upload files"
                            type="file"
                            accept={kind === "video" ? ".mp4" : ".zip"}
                            disabled={busy}
                            onChange={(e) => {
                                const selected = Array.from(e.target.files ?? []);
                                setFiles(selected);
                                if (!name && selected.length)
                                    setName(selected[0].name.replace(/\.[^.]+$/, ""));
                            }}
                        />
                    </label>
                </div>
                <label>
                    Display name
                    <input
                        required
                        maxLength={120}
                        value={name}
                        disabled={busy}
                        onChange={(e) => setName(e.target.value)}
                    />
                </label>
                {busy && (
                    <div>
                        <progress max={100} value={progress} />
                        <p className="muted">
                            Uploading {progress}% · validation continues on the board
                        </p>
                    </div>
                )}
                {error && (
                    <p className="error" role="alert">
                        {error}
                    </p>
                )}
                <footer className="actions">
                    <button type="button" onClick={onClose}>
                        {busy ? "Cancel upload" : "Cancel"}
                    </button>
                    <button className="primary" disabled={busy || !ready}>
                        {busy ? "Uploading…" : "Upload & validate"}
                    </button>
                </footer>
            </form>
        </Dialog>
    );
}
