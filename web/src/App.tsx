import { useEffect, useState } from "react";
import { useSnapshot } from "./lib/useSnapshot";
import { bytes } from "./lib/types";
import { Dashboard } from "./features/dashboard/Dashboard";
import { WarehousePage } from "./features/warehouse/WarehousePage";
import { UploadDialog } from "./components/UploadDialog";
type Page = "dashboard" | "videos" | "models";
function pageFromURL(): Page {
    const page = window.location.hash.slice(1);
    return page === "models" || page === "videos" ? page : "dashboard";
}
function NavIcon({ page }: { page: Page }) {
    return (
        <svg
            viewBox="0 0 24 24"
            width="24"
            height="24"
            fill="none"
            stroke="currentColor"
            strokeWidth="1.8"
            strokeLinecap="round"
            strokeLinejoin="round"
            aria-hidden="true"
        >
            {page === "dashboard" ? (
                <>
                    <rect x="3" y="6" width="12" height="12" rx="2" />
                    <path d="m15 10 6-3v10l-6-3" />
                </>
            ) : page === "videos" ? (
                <>
                    <rect x="3" y="4" width="18" height="16" rx="3" />
                    <path d="m10 8 6 4-6 4Z" />
                </>
            ) : (
                <>
                    <path d="m12 3 9 5v8l-9 5-9-5V8Zm0 9 9-4M12 12 3 8m9 4v9" />
                </>
            )}
        </svg>
    );
}
export function App() {
    const { data, connected, error, refresh } = useSnapshot();
    const [page, setPage] = useState<Page>(pageFromURL),
        [upload, setUpload] = useState<"video" | "model" | null>(null);
    useEffect(() => {
        const syncPage = () => {
            setPage(pageFromURL());
            setUpload(null);
        };
        window.addEventListener("hashchange", syncPage);
        return () => window.removeEventListener("hashchange", syncPage);
    }, []);
    function navigate(next: Page) {
        window.location.hash = next;
        setPage(next);
        setUpload(null);
    }
    const system = data?.system;
    return (
        <div className="app-shell">
            <aside className="sidebar">
                <a
                    className="brand"
                    href="#dashboard"
                    aria-label="Rockchip YOLO home"
                    title="Rockchip YOLO C++"
                    onClick={() => navigate("dashboard")}
                >
                    <span className="brand-mark">R</span>
                </a>
                <nav aria-label="Main navigation">
                    {(
                        [
                            ["dashboard", "Dashboard"],
                            ["videos", "Video warehouse"],
                            ["models", "Model warehouse"],
                        ] as const
                    ).map(([id, title]) => (
                        <button
                            key={id}
                            className={page === id ? "nav-item selected" : "nav-item"}
                            aria-current={page === id ? "page" : undefined}
                            aria-label={title}
                            title={title}
                            onClick={() => navigate(id)}
                        >
                            <NavIcon page={id} />
                            <span className="nav-tooltip" aria-hidden="true">
                                {title}
                            </span>
                        </button>
                    ))}
                </nav>
                <div className="sidebar-bottom">
                    <span className="board-mark" title="RK3576 / RK3588 · MPP / RGA / RKNPU">
                        RK
                    </span>
                </div>
            </aside>
            <div className="workspace">
                <main>
                    {error && (
                        <div className="error" role="alert">
                            {error}
                            <button onClick={() => void refresh()}>Retry</button>
                        </div>
                    )}
                    {!data ? (
                        <div className="empty-state">
                            <h2>Connecting to your Rockchip board…</h2>
                            <p>Check that the board is running and reachable on your network.</p>
                        </div>
                    ) : page === "dashboard" ? (
                        <Dashboard
                            data={data}
                            refresh={() => void refresh()}
                            onUpload={() => setUpload("video")}
                        />
                    ) : (
                        <WarehousePage
                            key={page}
                            kind={page === "videos" ? "video" : "model"}
                            data={data}
                            refresh={() => void refresh()}
                            onUpload={() => setUpload(page === "videos" ? "video" : "model")}
                        />
                    )}
                </main>
                <footer className="system-strip" aria-label="Board system status">
                    <div>
                        <span className="metric-label">CPU</span>
                        <strong>
                            {system?.cpu_percent == null
                                ? "—"
                                : `${system.cpu_percent.toFixed(1)}%`}
                        </strong>
                    </div>
                    <div>
                        <span className="metric-label">Memory</span>
                        <strong>
                            {system
                                ? `${bytes(system.memory_used)} / ${bytes(system.memory_total)}`
                                : "—"}
                        </strong>
                    </div>
                    <div>
                        <span className="metric-label">Temp</span>
                        <strong>
                            {system?.temperature_c == null
                                ? "—"
                                : `${system.temperature_c.toFixed(1)}°C`}
                        </strong>
                    </div>
                    <div>
                        <span className="metric-label">Free</span>
                        <strong>{system ? bytes(system.storage_free) : "—"}</strong>
                    </div>
                    <div
                        title={
                            system?.npu_load ?? "NPU load requires readable kernel debug counters"
                        }
                    >
                        <span className="metric-label">NPU</span>
                        <strong>{system?.npu_load ?? "Counter unavailable"}</strong>
                    </div>
                    <div className="connection">
                        <span className={`status-dot ${connected ? "" : "offline"}`} />
                        <span aria-live="polite">
                            {connected ? "Board connected" : "Reconnecting…"}
                        </span>
                    </div>
                </footer>
            </div>
            {upload && (
                <UploadDialog
                    kind={upload}
                    onClose={() => setUpload(null)}
                    onComplete={() => void refresh()}
                />
            )}
        </div>
    );
}
