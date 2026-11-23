"""Remake textures whose transparency must stay exact (scope / binocular overlays, alpha-kill masks).

The generic 4x pack (upscale.py) runs the alpha channel through a photo super-resolution model.
On thin anti-aliased line art that thickened lines, made them opaque and lost dotted rims; on
alpha-kill masks it moved alpha that must be exactly 0. This rebuilds alpha by constrained
reconstruction on the GPU instead:

  * Consistency (hard): every 4x4 block of the result averages to the original texel's alpha
    (alternating projections with the [0,1] bounds). So a 4x box downsample - mip 2 - gives the
    original back, and no line can get thicker, darker or more opaque than the source.
  * Exact ends (hard): an original alpha of 0 gives a block of exact 0s, 255 a block of 255s, and
    any other value never produces a 0 texel (alpha kill sees the same holes as the original).
  * Edge prior (soft): where the source has a real edge (full local contrast within 3x3 texels),
    values are pulled to the two local levels (the 3x3 min and max), so edges come out crisp at
    their sub-texel position; smooth gradients (contrast spread over a wider area) are left smooth.
    A small total-variation term keeps the edges as smooth curves instead of stair steps.

Colour: constant colour stays constant (the overlays are pure black); otherwise the colour channels
go through the super-resolution model as in upscale.py, alpha never does.
Output: PNG at 4x plus a report line per file with the measured consistency error.
"""
import argparse, json, pathlib, sys
import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image

S = 4
DEV = "cuda"


def pad(x, mode, p):
    return F.pad(x, (p, p, p, p), mode=mode)


def local_minmax(a, k, mode):
    p = k // 2
    ap = pad(a, mode, p)
    mx = F.max_pool2d(ap, k, 1)
    mn = -F.max_pool2d(-ap, k, 1)
    return mn, mx


def reconstruct(a8, tiling, iters=600, w_tv=0.02, w_bin=4.0, lr=0.02):
    """a8: HxW uint8 alpha. Returns (4H)x(4W) uint8."""
    mode = "circular" if tiling else "replicate"
    a = torch.from_numpy(a8.astype(np.float32) / 255).to(DEV)[None, None]
    H, W = a8.shape
    up = lambda t: F.interpolate(t, scale_factor=S, mode="nearest")

    # hard bounds per hi-res texel
    lo_b = torch.where(a == 0, 0.0, 1.0 / 255)
    hi_b = torch.where(a == 0, 0.0, 1.0)
    lo_b = torch.where(a == 1, 1.0, lo_b)
    lo_b, hi_b = up(lo_b), up(hi_b)

    # edge prior: local two levels, and where they apply
    mn3, mx3 = local_minmax(a, 3, mode)
    mn7, mx7 = local_minmax(a, 7, mode)
    c3, c7 = mx3 - mn3, mx7 - mn7
    edge = ((c3 / (c7 + 1e-6) - 0.6) / 0.3).clamp(0, 1) * ((c3 - 0.04) / 0.08).clamp(0, 1)
    # levels at hi-res: smooth them so the target levels do not step at texel borders
    L = F.interpolate(pad(mn3, mode, 1), scale_factor=S, mode="bilinear", align_corners=False)[..., S:-S, S:-S]
    Hh = F.interpolate(pad(mx3, mode, 1), scale_factor=S, mode="bilinear", align_corners=False)[..., S:-S, S:-S]
    E = F.interpolate(pad(edge, mode, 1), scale_factor=S, mode="bilinear", align_corners=False)[..., S:-S, S:-S]

    def project(x, n=4):
        for _ in range(n):
            r = a - F.avg_pool2d(x, S)
            x = (x + up(r)).clamp(min=0)
            x = torch.maximum(torch.minimum(x, hi_b), lo_b)
        return x

    # start: bicubic (smooth), made consistent
    x0 = F.interpolate(pad(a, mode, 2), scale_factor=S, mode="bicubic", align_corners=False)[..., 2 * S:-2 * S, 2 * S:-2 * S]
    x = project(x0.clamp(0, 1), 20).clone().requires_grad_(True)
    opt = torch.optim.Adam([x], lr=lr)
    for it in range(iters):
        opt.zero_grad()
        xp = pad(x, mode, 1)
        gx = xp[..., 1:-1, 2:] - xp[..., 1:-1, 1:-1]
        gy = xp[..., 2:, 1:-1] - xp[..., 1:-1, 1:-1]
        tv = torch.sqrt(gx * gx + gy * gy + 1e-4).mean()
        span = (Hh - L).clamp(min=1e-3)
        t = ((x - L) / span).clamp(0, 1)
        two_level = (E * (t * (1 - t)) * span * span).mean()
        loss = w_tv * tv + w_bin * two_level
        loss.backward()
        opt.step()
        with torch.no_grad():
            x.copy_(project(x))
    with torch.no_grad():
        x = project(x, 200)
        q = torch.round(x * 255)
        # rounding can move a block mean by up to 0.5/255; nudge the texels with the most
        # rounding slack so each block's sum lands on the original exactly where possible
        q = q.clamp(0, 255)
        q = torch.maximum(torch.minimum(q, hi_b * 255), torch.ceil(lo_b * 255 - 1e-4))
    return q[0, 0].to(torch.uint8).cpu().numpy()


class ColourModel:
    def __init__(self, path):
        from spandrel import ModelLoader
        self.net = ModelLoader().load_from_file(path).model.eval().cuda().half()

    def run(self, rgb, tiling):
        p = 16
        mode = "wrap" if tiling else "symmetric"
        big = np.pad(rgb, ((p, p), (p, p), (0, 0)), mode=mode)
        x = torch.from_numpy(np.ascontiguousarray(big.transpose(2, 0, 1)[None])).cuda().half()
        with torch.inference_mode():
            y = self.net(x).float().clamp_(0, 1)[0].permute(1, 2, 0).cpu().numpy()
        return y[p * S:-p * S, p * S:-p * S]


def check(a8, q):
    H, W = a8.shape
    blocks = q.reshape(H, S, W, S).astype(np.float32)
    mean = blocks.mean(axis=(1, 3))
    err = np.abs(mean - a8)
    zero_ok = bool((blocks.max(axis=(1, 3))[a8 == 0] == 0).all())
    nonzero_ok = bool((blocks.min(axis=(1, 3))[a8 > 0] > 0).all())
    full_ok = bool((blocks.min(axis=(1, 3))[a8 == 255] == 255).all())
    return {"mean_err_max": float(err.max()), "mean_err_avg": float(err.mean()),
            "zero_exact": zero_ok, "no_new_holes": nonzero_ok, "opaque_exact": full_ok}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dst", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--iters", type=int, default=600)
    ap.add_argument("--tv", type=float, default=0.02)
    ap.add_argument("--bin", type=float, default=4.0)
    ap.add_argument("--lr", type=float, default=0.02)
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    src, dst = pathlib.Path(args.src), pathlib.Path(args.dst)
    model = None
    report = []
    for f in sorted(src.rglob("*.png")):
        rel = f.relative_to(src)
        if args.only and args.only not in f.name:
            continue
        tiling = rel.parts[1] not in ("hud", "interface")
        im = np.asarray(Image.open(f).convert("RGBA"))
        rgb, a8 = im[..., :3], im[..., 3]
        H, W = a8.shape
        if (rgb == rgb[0, 0]).all():
            rgb_up = np.broadcast_to(rgb[0, 0], (H * S, W * S, 3)).copy()
            how = "constant colour"
        else:
            model = model or ColourModel(args.model)
            rgb_up = (np.clip(model.run(rgb.astype(np.float32) / 255, tiling), 0, 1) * 255 + 0.5).astype(np.uint8)
            how = "model colour"
        a_up = reconstruct(a8, tiling, args.iters, args.tv, args.bin, args.lr)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(np.dstack([rgb_up, a_up]), "RGBA").save(out)
        r = {"file": str(rel).replace("\\", "/"), "colour": how, **check(a8, a_up)}
        report.append(r)
        print(json.dumps(r), flush=True)
    (dst / "remake_report.json").write_text(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
