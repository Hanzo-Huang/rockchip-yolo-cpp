import { useState } from "react";
import { api } from "../../lib/api";
import { active, type Channel, type Snapshot } from "../../lib/types";
import { ChannelCard } from "./ChannelCard";
import { ChannelEditor } from "./ChannelEditor";
import { ConfirmDelete } from "../../components/ConfirmDelete";
import { Dialog } from "../../components/Dialog";
export function Dashboard({
    data,
    refresh,
    onUpload,
}: {
    data: Snapshot;
    refresh: () => void;
    onUpload: () => void;
}) {
    const [editor, setEditor] = useState<Channel | "new" | null>(null),
        [deleting, setDeleting] = useState<Channel | null>(null),
        [focus, setFocus] = useState<string | null>(null);
    const focused = data.channels.find((channel) => channel.id === focus);
    const running = data.channels.filter((c) => active(c.runtime.state)).length;
    const card = (channel: Channel, large = false) => (
        <ChannelCard
            key={channel.id}
            channel={channel}
            model={data.models.find((m) => m.id === channel.model_id)}
            video={data.videos.find((v) => v.id === channel.video_id)}
            onEdit={() => {
                setFocus(null);
                setEditor(channel);
            }}
            onDelete={() => setDeleting(channel)}
            onFocus={() => setFocus(channel.id)}
            onRefresh={refresh}
            focused={large}
        />
    );
    return (
        <>
            <div className="page-heading">
                <div>
                    <h1>
                        Live <span className="count">{data.channels.length}</span>
                    </h1>
                    <p>{running} active · Rockchip YOLO</p>
                </div>
                <button className="primary" onClick={() => setEditor("new")}>
                    ＋ Add channel
                </button>
            </div>
            {data.channels.length ? (
                <div className="channel-grid">{data.channels.map((channel) => card(channel))}</div>
            ) : (
                <div className="empty-state">
                    <h2>Your monitoring starts here</h2>
                    <p>
                        Upload a video and model to their warehouses, then add your first channel.
                    </p>
                    <button className="primary" onClick={() => setEditor("new")}>
                        Add first channel
                    </button>
                </div>
            )}
            {editor && (
                <ChannelEditor
                    channel={editor === "new" ? undefined : editor}
                    data={data}
                    onClose={() => setEditor(null)}
                    onSaved={refresh}
                    onUpload={onUpload}
                />
            )}
            {deleting && (
                <ConfirmDelete
                    name={deleting.name}
                    onClose={() => setDeleting(null)}
                    onConfirm={async () => {
                        await api(`channels/${deleting.id}`, "DELETE", {
                            confirm_name: deleting.name,
                        });
                        refresh();
                    }}
                />
            )}
            {focused && (
                <Dialog title={focused.name} wide onClose={() => setFocus(null)}>
                    {card(focused, true)}
                </Dialog>
            )}
        </>
    );
}
