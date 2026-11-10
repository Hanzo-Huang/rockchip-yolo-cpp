import { useEffect, useState } from "react";
import { api } from "../../lib/api";
import type { Asset } from "../../lib/types";
import { Dialog } from "../../components/Dialog";
export function RenameDialog({
    asset,
    onClose,
    refresh,
}: {
    asset: Asset;
    onClose: () => void;
    refresh: () => void;
}) {
    const [name, setName] = useState(asset.name),
        [error, setError] = useState(""),
        [busy, setBusy] = useState(false);
    return (
        <Dialog title="Rename item" onClose={onClose}>
            <form
                onSubmit={async (e) => {
                    e.preventDefault();
                    setBusy(true);
                    try {
                        await api(`${asset.kind}s/${asset.id}`, "PATCH", { name });
                        refresh();
                        onClose();
                    } catch (e) {
                        setError(String(e));
                        setBusy(false);
                    }
                }}
            >
                <label>
                    Display name
                    <input
                        autoFocus
                        required
                        maxLength={120}
                        value={name}
                        onChange={(e) => setName(e.target.value)}
                    />
                </label>
                {error && (
                    <p className="error" role="alert">
                        {error}
                    </p>
                )}
                <footer className="actions">
                    <button type="button" onClick={onClose}>
                        Cancel
                    </button>
                    <button className="primary" disabled={busy}>
                        Save name
                    </button>
                </footer>
            </form>
        </Dialog>
    );
}
export function ModelDetails({
    asset,
    onClose,
    refresh,
}: {
    asset: Asset;
    onClose: () => void;
    refresh: () => void;
}) {
    const [yaml, setYAML] = useState("Loading metadata…"),
        [error, setError] = useState(""),
        [revalidationError, setRevalidationError] = useState(""),
        [revalidating, setRevalidating] = useState(false);
    useEffect(() => {
        const controller = new AbortController();
        void api<{ yaml: string }>(
            `models/${asset.id}/metadata`,
            "GET",
            undefined,
            controller.signal,
        )
            .then((value) => setYAML(value.yaml))
            .catch((e) => {
                if (!controller.signal.aborted) setError(String(e));
            });
        return () => controller.abort();
    }, [asset.id]);
    return (
        <Dialog title={`Model · ${asset.name}`} wide onClose={onClose}>
            <p className="notice">
                {asset.info.reason} Runtime compatibility does not certify model accuracy.
            </p>
            <div className="model-facts">
                <span>{asset.info.task ?? "detect"}</span>
                <span>
                    {asset.info.width} × {asset.info.height} input
                </span>
                <span>{asset.info.names?.length ?? 0} classes</span>
                <span>{String(asset.info.postprocessor ?? "Unknown output")}</span>
            </div>
            {error && <p className="error">{error}</p>}
            <pre className="metadata">{yaml}</pre>
            {revalidationError && (
                <p className="error" role="alert">
                    {revalidationError}
                </p>
            )}
            <footer className="actions">
                {!asset.info.toolkit_version && (
                    <button
                        disabled={revalidating}
                        onClick={async () => {
                            setRevalidationError("");
                            setRevalidating(true);
                            try {
                                await api(`models/${asset.id}/revalidate`, "POST");
                                refresh();
                            } catch (e) {
                                setRevalidationError(String(e));
                            } finally {
                                setRevalidating(false);
                            }
                        }}
                    >
                        {revalidating ? "Revalidating…" : "Revalidate model"}
                    </button>
                )}
                <a className="button" href={`/api/v1/models/${asset.id}/download`}>
                    Download model bundle
                </a>
            </footer>
        </Dialog>
    );
}
