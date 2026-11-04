import { useCallback, useEffect, useState } from "react";
import { api, socketURL } from "./api";
import type { Snapshot } from "./types";
export function useSnapshot() {
    const [data, setData] = useState<Snapshot>();
    const [connected, setConnected] = useState(false);
    const [error, setError] = useState("");
    const refresh = useCallback(async () => {
        try {
            const value = await api<Snapshot>("state");
            setData(value);
            setError("");
        } catch (e) {
            setError(e instanceof Error ? e.message : "Connection failed");
        }
    }, []);
    useEffect(() => {
        let disposed = false,
            timer: ReturnType<typeof setTimeout>,
            socket: WebSocket;
        const connect = () => {
            if (disposed) return;
            socket = new WebSocket(socketURL("/ws/events"));
            socket.onopen = () => {
                if (!disposed) setConnected(true);
            };
            socket.onmessage = (event) => {
                if (disposed) return;
                try {
                    const value: Snapshot = JSON.parse(event.data);
                    setData(value);
                    setError("");
                } catch {
                    /* reconnect supplies another snapshot */
                }
            };
            socket.onclose = () => {
                if (!disposed) {
                    setConnected(false);
                    timer = setTimeout(connect, 2000);
                }
            };
        };
        void refresh();
        connect();
        return () => {
            disposed = true;
            clearTimeout(timer);
            socket?.close();
        };
    }, [refresh]);
    return { data, connected, error, refresh };
}
