import { useMemo, useState } from "react";

const pageSize = 48;

export function ClassPicker({
    names,
    selected,
    onChange,
    disabled,
}: {
    names: string[];
    selected: number[] | null;
    onChange: (ids: number[] | null) => void;
    disabled: boolean;
}) {
    const [query, setQuery] = useState("");
    const [page, setPage] = useState(0);
    const matches = useMemo(() => {
        const search = query.trim().toLowerCase();
        return names
            .map((name, id) => ({ name, id }))
            .filter(({ name }) => name.toLowerCase().includes(search));
    }, [names, query]);
    const chosen = useMemo(() => new Set(selected), [selected]);
    const pages = Math.max(1, Math.ceil(matches.length / pageSize));
    const currentPage = Math.min(page, pages - 1);
    const visible = matches.slice(currentPage * pageSize, (currentPage + 1) * pageSize);
    const count = selected === null ? `All ${names.length}` : `${selected.length} of ${names.length}`;

    function toggle(id: number, checked: boolean) {
        const next = selected === null ? new Set(names.map((_, index) => index)) : new Set(chosen);
        if (checked) next.add(id);
        else next.delete(id);
        onChange(next.size === names.length ? null : [...next]);
    }

    return (
        <details className="class-picker">
            <summary>Detection classes · {count}</summary>
            <div className="class-picker-controls">
                <label>
                    Search classes
                    <input
                        type="search"
                        value={query}
                        onChange={(event) => {
                            setQuery(event.target.value);
                            setPage(0);
                        }}
                        disabled={disabled}
                    />
                </label>
                <button
                    type="button"
                    onClick={() => onChange(null)}
                    disabled={disabled || selected === null}
                >
                    Select all
                </button>
                <button
                    type="button"
                    onClick={() => onChange([])}
                    disabled={disabled || selected?.length === 0}
                >
                    Clear all
                </button>
            </div>
            {matches.length ? (
                <div
                    className="class-picker-list"
                    key={currentPage}
                    role="group"
                    aria-label="Detection classes"
                >
                    {visible.map(({ name, id }) => (
                        <label className="check" key={id}>
                            <input
                                type="checkbox"
                                checked={selected === null || chosen.has(id)}
                                onChange={(event) => toggle(id, event.target.checked)}
                                disabled={disabled}
                            />
                            <span>{name}</span>
                        </label>
                    ))}
                </div>
            ) : (
                <p className="muted">No classes match. Try a different search.</p>
            )}
            {pages > 1 && (
                <div className="class-picker-pages">
                    <button
                        type="button"
                        disabled={disabled || currentPage === 0}
                        onClick={() => setPage(currentPage - 1)}
                    >
                        Previous
                    </button>
                    <span className="muted" aria-live="polite">
                        Page {currentPage + 1} of {pages}
                    </span>
                    <button
                        type="button"
                        disabled={disabled || currentPage + 1 === pages}
                        onClick={() => setPage(currentPage + 1)}
                    >
                        Next
                    </button>
                </div>
            )}
        </details>
    );
}
