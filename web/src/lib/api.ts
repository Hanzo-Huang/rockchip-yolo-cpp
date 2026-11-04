export async function api<T = unknown>(
    path: string,
    method = "GET",
    body?: unknown,
    signal?: AbortSignal,
): Promise<T> {
    const response = await fetch(`/api/v1/${path}`, {
        method,
        signal,
        headers: { "Content-Type": "application/json" },
        body: body === undefined ? undefined : JSON.stringify(body),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || `Request failed (${response.status})`);
    return result as T;
}
export const socketURL = (path: string) =>
    `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}${path}`;
export async function upload(
    kind: string,
    name: string,
    files: File[],
    progress: (value: number) => void,
    signal: AbortSignal,
) {
    const job = await api<{ id: string }>(
        "uploads",
        "POST",
        { kind, name, files: files.map((file) => ({ name: file.name, size: file.size })) },
        signal,
    );
    const total = files.reduce((sum, file) => sum + file.size, 0);
    let received = 0;
    try {
        for (let index = 0; index < files.length; index++) {
            const file = files[index];
            for (let offset = 0; offset < file.size; offset += 4 * 1024 ** 2) {
                const chunk = file.slice(offset, offset + 4 * 1024 ** 2);
                const response = await fetch(`/api/v1/uploads/${job.id}/files/${index}`, {
                    method: "PUT",
                    signal,
                    headers: {
                        "X-Upload-Offset": String(offset),
                        "Content-Type": "application/octet-stream",
                    },
                    body: chunk,
                });
                if (!response.ok) throw new Error((await response.json()).error || "Upload failed");
                received += chunk.size;
                progress(Math.round((received / total) * 100));
            }
        }
        await api(`uploads/${job.id}/complete`, "POST", {}, signal);
        return job.id;
    } catch (error) {
        void api(`uploads/${job.id}`, "DELETE").catch(() => {});
        throw error;
    }
}
