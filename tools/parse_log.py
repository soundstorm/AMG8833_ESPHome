#!/usr/bin/env python3
"""Convert an `esphome logs` capture of the amg8833 component into CSV files and
optionally render the frames as a heatmap animation.

Usage:
    esphome logs device.yaml > frames.log        # with logger level DEBUG and log_frames: true
    python3 tools/parse_log.py frames.log        # writes frames.csv and crossings.csv
    python3 tools/parse_log.py frames.log --gif  # additionally writes frames.gif (needs numpy, matplotlib, pillow)

frames.csv columns:    ms, warm, centroid, tracking, t_0 ... t_63 (degrees Celsius, row by row)
crossings.csv columns: start, end, travel, duration_ms, frames, max_warm, result
warm = -1 marks warm-up frames, centroid = -1 means no warm area.
"""
import argparse
import csv
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def parse(path):
    frames, crossings = [], []
    with open(path, errors="replace") as f:
        for line in f:
            line = ANSI.sub("", line)
            if "FRAME," in line:
                parts = line.split("FRAME,", 1)[1].strip().split(",")
                if len(parts) == 4 + 64:
                    ms, warm, centroid, tracking = parts[:4]
                    temps = [int(v) * 0.25 for v in parts[4:]]
                    # Firmware before the buffer fix cut off the tracking flag when the centroid was -1.00.
                    frames.append([int(ms), int(warm), float(centroid), int(tracking or 0)] + temps)
            elif "CROSSING," in line:
                parts = line.split("CROSSING,", 1)[1].strip().split(",")
                if len(parts) == 7:
                    crossings.append(parts)
    return frames, crossings


def write_csv(path, header, rows):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def render_gif(frames, path, vmin, vmax):
    import matplotlib.pyplot as plt
    import numpy as np
    from matplotlib.animation import FuncAnimation, PillowWriter

    fig, ax = plt.subplots(figsize=(4, 4))
    img = ax.imshow(np.zeros((8, 8)), vmin=vmin, vmax=vmax, cmap="inferno", interpolation="bicubic")
    title = ax.set_title("")
    ax.axis("off")

    def update(i):
        fr = frames[i]
        img.set_data(np.array(fr[4:]).reshape(8, 8))
        title.set_text(f"{fr[0] / 1000:.1f} s  warm={fr[1]}  pos={fr[2]:.1f}")
        return img, title

    FuncAnimation(fig, update, frames=len(frames), interval=100).save(path, writer=PillowWriter(fps=10))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--gif", action="store_true", help="also render frames.gif")
    ap.add_argument("--vmin", type=float, default=15)
    ap.add_argument("--vmax", type=float, default=35)
    args = ap.parse_args()

    frames, crossings = parse(args.log)
    if not frames and not crossings:
        sys.exit("No FRAME/CROSSING lines found. Use logger level DEBUG and log_frames: true.")
    write_csv("frames.csv", ["ms", "warm", "centroid", "tracking"] + [f"t_{i}" for i in range(64)], frames)
    write_csv("crossings.csv", ["start", "end", "travel", "duration_ms", "frames", "max_warm", "result"], crossings)
    print(f"{len(frames)} frames -> frames.csv, {len(crossings)} crossings -> crossings.csv")
    if args.gif and frames:
        render_gif(frames, "frames.gif", args.vmin, args.vmax)
        print("frames.gif written")


if __name__ == "__main__":
    main()
