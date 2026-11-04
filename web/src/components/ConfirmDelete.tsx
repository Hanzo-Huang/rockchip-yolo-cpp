import { useState } from "react";
import { Dialog } from "./Dialog";
export function ConfirmDelete({
    name,
    references = [],
    onClose,
    onConfirm,
}: {
    name: string;
    references?: { name: string }[];
    onClose: () => void;
    onConfirm: () => Promise<void>;
}) {
    const [stage, setStage] = useState(1),
        [value, setValue] = useState(""),
        [busy, setBusy] = useState(false),
        [error, setError] = useState("");
    return (
        <Dialog title="Delete item" onClose={onClose}>
            {references.length ? (
                <p className="notice">
                    Assigned to {references.map((item) => item.name).join(", ")}. Remove these
                    assignments before deleting.
                </p>
            ) : (
                <>
                    <p>
                        Delete <strong>{name}</strong>? Warehouse files are moved to recoverable
                        board-side trash. Channel recordings are preserved.
                    </p>
                    {stage === 2 && (
                        <label>
                            Type the exact name to confirm
                            <input
                                autoFocus
                                value={value}
                                onChange={(e) => setValue(e.target.value)}
                                placeholder={name}
                            />
                        </label>
                    )}
                    {error && (
                        <p className="error" role="alert">
                            {error}
                        </p>
                    )}
                    <footer className="actions">
                        <button onClick={onClose}>Cancel</button>
                        <button
                            className="danger"
                            disabled={busy || (stage === 2 && value !== name)}
                            onClick={async () => {
                                if (stage === 1) {
                                    setStage(2);
                                    return;
                                }
                                setBusy(true);
                                try {
                                    await onConfirm();
                                    onClose();
                                } catch (e) {
                                    setError(String(e));
                                    setBusy(false);
                                }
                            }}
                        >
                            {busy
                                ? "Deleting…"
                                : stage === 1
                                  ? "Continue to confirmation"
                                  : "Confirm delete"}
                        </button>
                    </footer>
                </>
            )}
        </Dialog>
    );
}
