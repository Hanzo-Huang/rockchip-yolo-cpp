import type { InferenceResult, ResultRaster } from "./inference";

const palette = ["#57e3bd", "#ffbc66", "#7fb5ff", "#eaa1f4", "#ff8b93", "#d0de6a"];
const skeleton = [
    [15, 13],
    [13, 11],
    [16, 14],
    [14, 12],
    [11, 12],
    [5, 11],
    [6, 12],
    [5, 6],
    [5, 7],
    [6, 8],
    [7, 9],
    [8, 10],
    [1, 2],
    [0, 1],
    [0, 2],
    [1, 3],
    [2, 4],
    [3, 5],
    [4, 6],
];
const rasterCache = new WeakMap<ResultRaster, HTMLCanvasElement>();

// Rasterize a compact result once, rather than rebuilding its pixels every video frame.
function rasterCanvas(raster: ResultRaster, task: string): HTMLCanvasElement | undefined {
    const cached = rasterCache.get(raster);
    if (cached) return cached;
    const { width, height, runs } = raster;
    if (
        !Number.isInteger(width) ||
        !Number.isInteger(height) ||
        width < 1 ||
        height < 1 ||
        width * height > 65536 ||
        runs.length % 2
    )
        return;
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext("2d");
    if (!context) return;
    const pixels = context.createImageData(width, height);
    let offset = 0;
    for (let i = 0; i < runs.length; i += 2) {
        const value = runs[i],
            count = runs[i + 1];
        if (
            !Number.isInteger(value) ||
            value < 0 ||
            !Number.isInteger(count) ||
            count <= 0 ||
            offset + count > width * height
        )
            return;
        let rgb: number[];
        if (task === "depth") {
            const fraction = Math.max(0, Math.min(1, value / 65535));
            rgb = [
                Math.round(255 * fraction),
                Math.round(220 * (1 - Math.abs(2 * fraction - 1))),
                Math.round(255 * (1 - fraction)),
            ];
        } else {
            const color = palette[value % palette.length];
            rgb = [1, 3, 5].map((position) => parseInt(color.slice(position, position + 2), 16));
        }
        for (let j = 0; j < count; ++j, ++offset) {
            const p = offset * 4;
            pixels.data[p] = rgb[0];
            pixels.data[p + 1] = rgb[1];
            pixels.data[p + 2] = rgb[2];
            pixels.data[p + 3] =
                (task === "segment" && value === 0) || (task === "depth" && value === 65536)
                    ? 0
                    : 255;
        }
    }
    if (offset !== width * height) return;
    context.putImageData(pixels, 0, 0);
    rasterCache.set(raster, canvas);
    return canvas;
}

function caption(context: CanvasRenderingContext2D, text: string, x: number, y: number) {
    const left = Math.max(
        0,
        Math.min(x, context.canvas.width - context.measureText(text).width - 10),
    );
    const top = Math.max(18, y);
    context.fillStyle = "rgba(12, 28, 30, 0.9)";
    context.fillRect(
        left,
        top - 17,
        Math.min(context.canvas.width, context.measureText(text).width + 10),
        20,
    );
    context.fillStyle = "#e8fff6";
    context.fillText(text, left + 5, top - 3);
}

export function drawResults(
    context: CanvasRenderingContext2D,
    result: InferenceResult,
    labels?: string[],
) {
    if (result.source_width <= 0 || result.source_height <= 0) return;
    const sx = context.canvas.width / result.source_width,
        sy = context.canvas.height / result.source_height;
    context.save();
    context.font = "12px sans-serif";
    context.lineWidth = 1.5;
    const drawRaster = (raster: ResultRaster, task: string) => {
        const image = rasterCanvas(raster, task);
        if (!image || raster.box.length !== 4 || !raster.box.every(Number.isFinite)) return;
        const [x1, y1, x2, y2] = raster.box;
        context.globalAlpha = task === "depth" ? 0.75 : 0.4;
        context.imageSmoothingEnabled = task === "depth";
        context.drawImage(image, x1 * sx, y1 * sy, (x2 - x1) * sx, (y2 - y1) * sy);
        context.globalAlpha = 1;
    };
    if (result.dense) {
        drawRaster(result.dense, result.task ?? "semantic");
        if (result.task === "depth") {
            const maximum = result.dense.offset + 65535 * result.dense.scale;
            caption(
                context,
                `Depth · ${result.dense.offset.toFixed(2)}–${maximum.toFixed(2)} · blue near, red far`,
                6,
                22,
            );
        } else {
            const counts = new Map<number, number>();
            for (let i = 0; i < result.dense.runs.length; i += 2) {
                const id = result.dense.runs[i];
                counts.set(id, (counts.get(id) ?? 0) + result.dense.runs[i + 1]);
            }
            const legend = [...counts].sort((a, b) => b[1] - a[1]).slice(0, 4);
            legend.forEach(([id], index) => {
                context.fillStyle = palette[id % palette.length];
                context.fillRect(6, 7 + index * 22, 10, 10);
                caption(context, labels?.[id] ?? `class ${id}`, 20, 22 + index * 22);
            });
        }
    }
    for (const item of result.detections) {
        if (item.mask) drawRaster(item.mask, "segment");
        const [x1, y1, x2, y2] = item.box;
        context.strokeStyle = palette[item.class_id % palette.length];
        if (item.polygon?.length === 4) {
            context.beginPath();
            item.polygon.forEach(([x, y], index) =>
                index ? context.lineTo(x * sx, y * sy) : context.moveTo(x * sx, y * sy),
            );
            context.closePath();
            context.stroke();
        } else context.strokeRect(x1 * sx, y1 * sy, (x2 - x1) * sx, (y2 - y1) * sy);
        if (item.keypoints) {
            const points = item.keypoints;
            if (points.length === 17 && labels?.[item.class_id] === "person") {
                for (const [a, b] of skeleton) {
                    if (points[a][2] < 0.5 || points[b][2] < 0.5) continue;
                    context.beginPath();
                    context.moveTo(points[a][0] * sx, points[a][1] * sy);
                    context.lineTo(points[b][0] * sx, points[b][1] * sy);
                    context.stroke();
                }
            }
            for (const [x, y, confidence] of points) {
                if (confidence < 0.5) continue;
                context.beginPath();
                context.arc(x * sx, y * sy, 2.5, 0, Math.PI * 2);
                context.fillStyle = "#fff";
                context.fill();
            }
        }
        caption(
            context,
            `${labels?.[item.class_id] ?? `class ${item.class_id}`} ${(item.confidence * 100).toFixed(0)}%`,
            x1 * sx,
            y1 * sy,
        );
    }
    result.classification?.forEach((item, index) => {
        caption(
            context,
            `${index + 1}. ${labels?.[item.class_id] ?? `class ${item.class_id}`} ${(item.confidence * 100).toFixed(1)}%`,
            6,
            22 + index * 23,
        );
    });
    context.restore();
}
