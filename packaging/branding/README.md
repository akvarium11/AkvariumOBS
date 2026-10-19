# Branding assets

Generated, not hand-drawn — regenerate instead of editing the PNGs.

```bash
python packaging/branding/make-banner.py
```

| File              | Size      | Use                                  |
| ----------------- | --------- | ------------------------------------ |
| `banner.png`      | 1667×458  | README header / repo site (EN copy)  |
| `banner-ru.png`   | 1667×458  | same, Russian copy                   |
| `banner@2x.png`   | 3334×916  | HiDPI screens, printing, slides      |

Design rules the script enforces:

- **strictly monochrome** — palette is neutral grey only (`max(R,G,B) - min(R,G,B) == 0` on every pixel);
- **rounded 28 px corners with real alpha** — the PNG is RGBA, so the banner drops onto light or dark
  backgrounds without a white box around it (radius is `CORNER_RADIUS`, supersampled for clean edges);
- icon taken straight from `frontend/forms/images/obs.png` (its alpha is preserved), laid on a light squircle;
- background = the icon silhouette blown up, blurred and used as a soft aura, plus featureless light
  hazes, film grain and a vignette;
- text: Segoe UI Bold (title), Segoe UI (subtitle), Segoe UI Semibold (pills).

Edit the `EN` / `RU` dicts at the bottom of the script to change copy, and the palette constants at the
top to retune contrast. Everything is rendered at 2× and downsampled, so edges and text stay clean.
