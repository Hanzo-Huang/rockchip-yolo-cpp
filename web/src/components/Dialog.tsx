import { useEffect, useId, useRef, type ReactNode } from "react";
export function Dialog({
    title,
    onClose,
    children,
    wide = false,
}: {
    title: string;
    onClose: () => void;
    children: ReactNode;
    wide?: boolean;
}) {
    const ref = useRef<HTMLDialogElement>(null),
        id = useId();
    const close = useRef(onClose);
    close.current = onClose;
    useEffect(() => {
        const previous = document.activeElement as HTMLElement | null;
        const dialog = ref.current;
        dialog?.showModal();
        return () => {
            dialog?.close();
            previous?.focus();
        };
    }, []);
    return (
        <dialog
            ref={ref}
            className={wide ? "dialog wide" : "dialog"}
            aria-labelledby={id}
            onCancel={(e) => {
                // Cancelling a native file picker bubbles from its input. Only a
                // cancellation of this dialog itself should dismiss the card.
                if (e.target !== e.currentTarget) return;
                e.preventDefault();
                close.current();
            }}
        >
            <header className="dialog-head">
                <h2 id={id}>{title}</h2>
                <button className="icon-button" aria-label="Close dialog" onClick={onClose}>
                    ×
                </button>
            </header>
            {children}
        </dialog>
    );
}
