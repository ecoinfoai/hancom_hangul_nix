# Hancom Hangul (Hwp 2020) for Nix — native, no Docker

Packages `hoffice_hwp_2020_amd64.deb` (11.20.0.989) from the Hancom CDN directly
with `autoPatchelfHook` (same approach as `viber` in nixpkgs).

## Run

```shell
nix run github:erdosxx/hancom_hangul_nix            # or: nix run .
nix run . -- document.hwp
```

## Outputs

| Output                     | Description                                                     |
| -------------------------- | --------------------------------------------------------------- |
| `packages.*.default`       | = `hancom-hwp` (fcitx5)                                          |
| `packages.*.hancom-hwp`    | Uses nixpkgs Qt 5.15 + `fcitx5-qt` plugin (**fcitx5 input**)     |
| `packages.*.hancom-hwp-ibus` | Uses bundled Qt 5.11 + its ibus plugin (**ibus input**)        |
| `apps.*.default`           | Runs `hwp`                                                      |
| `overlays.default`         | Adds `pkgs.hancom-hwp`                                          |

The package installs `bin/hwp`, a `.desktop` entry, icons and MIME types.

## Use in NixOS / home-manager

```nix
inputs.hancom-hwp.url = "github:erdosxx/hancom_hangul_nix";

# package
home.packages = [ inputs.hancom-hwp.packages.${system}.default ];
# or keep the old style used with the docker flake
run_hwp = inputs.hancom-hwp.apps.${system}.default.program;
```

Customize with `override`:

```nix
inputs.hancom-hwp.packages.${system}.default.override {
  scaleFactor = "1.2";        # QT_SCALE_FACTOR (default "1"; env var still wins)
  inputMethod = "fcitx5";     # or "ibus"
  hwpLocale   = "ko_KR.UTF-8";
}
```

With the overlay, `pkgs.hancom-hwp` is unfree, so allow it
(`nixpkgs.config.allowUnfree` or an `allowUnfreePredicate`).

## Bundled Korean fonts for other apps (`hancom-fonts`)

Hwp ships its fonts inside its install directory, so other programs can't see
them. For example, IeumOffice reports "이 PC에 없는 글꼴 … 함초롬돋움, 함초롬바탕"
for HWP documents. `hancom-fonts` copies them into `share/fonts`:

```nix
# home-manager (with fonts.fontconfig.enable = true)
home.packages = [ inputs.hancom-hwp.packages.${system}.hancom-fonts ];
# or NixOS
fonts.packages = [ inputs.hancom-hwp.packages.${system}.hancom-fonts ];
```

`fontSets` picks the fonts by family name (default `[ "hcr" ]`):

| set      | fonts                                                                                   | size   |
| -------- | --------------------------------------------------------------------------------------- | ------ |
| `hcr`    | 함초롬바탕 / 함초롬돋움 (HCR Batang / Dotum, incl. Ext): the default fonts of HWP documents | 196 MB |
| `hancom` | all Hancom fonts: 함초롬, 한컴 고딕, 한컴산뜻돋움, 한컴바탕/돋움, 한컴 윤고딕/윤체/…, Haan symbol fonts | 454 MB |
| `public` | free Korean public fonts in the bundle: 경기천년, KBIZ한마음, 문체부, 전주 완판본, 나눔손글씨 붓 | 134 MB |
| `all`    | every font in `Shared/TTF`, **including Microsoft fonts** (Arial, Calibri, Times New Roman, Tahoma, Verdana, Malgun Gothic, …) | 760 MB |

```nix
inputs.hancom-hwp.packages.${system}.hancom-fonts.override { fontSets = [ "hancom" "public" ]; }
```

- `all` is not the default. System-wide, the Microsoft fonts take precedence
  over metric-compatible fonts (Liberation, Carlito) in browsers and other apps.
- The fonts are copied, not linked, so this package doesn't keep the 1.7 GB Hwp
  install in your closure.
- Building it needs the Hwp `.deb`, which is downloaded or taken from the store.
- The fonts come with Hancom's unfree package, so treat them as licensed for use
  with Hwp. Don't redistribute them.

## How it works

- The `.deb` is fetched with the `Referer` header the Hancom CDN requires.
- `autoPatchelfHook` patches all ELF files. Debian's `libcurl-gnutls.so.4` is
  provided as a symlink to `curlWithGnuTls`.
- **fcitx5 mode (default):** hwp uses only the public Qt5 ABI (`Qt_5`/`Qt_5.11`
  symbol versions), so the bundled Qt 5.11 is removed and nixpkgs Qt 5.15 is used.
  This lets the real `fcitx5-qt` Qt5 plugin load (`QT_IM_MODULE=fcitx`). A plugin
  built for Qt 5.15 cannot load into the bundled Qt 5.11.
- **ibus mode:** keeps the bundled Qt 5.11. Plugins that need Qt modules not in
  the bundle (Wayland, QML, eglfs, webgl) are removed.
- The wrapper forces the X11 backend (`QT_QPA_PLATFORM=xcb`) and isolates
  `QT_PLUGIN_PATH`. It sets a full `glibcLocales` archive (`LOCALE_ARCHIVE_2_27`)
  so `ko_KR.UTF-8` works, which keeps Korean file names and the Korean UI.

Note: the wrapper shell may print `setlocale: LC_ALL: cannot change locale`.
This message comes from bash itself and is harmless; hwp gets the right locale.
