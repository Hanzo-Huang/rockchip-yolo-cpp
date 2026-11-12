import { useState } from "react";
import { Dialog } from "../../components/Dialog";
import { api } from "../../lib/api";

export function HuggingFaceImport({
    onClose,
    onQueued,
}: {
    onClose: () => void;
    onQueued: () => void;
}) {
    const [url, setURL] = useState("");
    const [name, setName] = useState("");
    const [error, setError] = useState("");
    const [busy, setBusy] = useState(false);

    async function submit() {
        setBusy(true);
        setError("");
        try {
            await api("models/hugging-face", "POST", { name: name.trim(), url: url.trim() });
            onQueued();
            onClose();
        } catch (e) {
            setError(e instanceof Error ? e.message : "Could not queue the model import");
            setBusy(false);
        }
    }

    return (
        <Dialog
            title="Import from Hugging Face"
            onClose={() => {
                if (!busy) onClose();
            }}
        >
            <form
                onSubmit={(event) => {
                    event.preventDefault();
                    void submit();
                }}
            >
                <label>
                    Hugging Face file link
                    <input
                        type="url"
                        value={url}
                        onChange={(event) => setURL(event.target.value)}
                        placeholder="https://huggingface.co/…/model.rknn"
                        required
                        maxLength={2048}
                        spellCheck={false}
                        disabled={busy}
                    />
                    <small>
                        Choose a public RKNN file with metadata.yaml beside it, or a ZIP containing both.
                    </small>
                </label>
                <label>
                    Model name
                    <input
                        value={name}
                        onChange={(event) => setName(event.target.value)}
                        placeholder="e.g. YOLO26n Seg · RK3576 INT8"
                        required
                        maxLength={120}
                        disabled={busy}
                    />
                </label>
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
                        {busy ? "Queuing…" : "Import model"}
                    </button>
                </footer>
            </form>
        </Dialog>
    );
}
