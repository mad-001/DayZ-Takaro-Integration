# Publishing to the Steam Workshop

The mod is server-side only, but it's still convenient to host it on the Workshop so server admins can pull it via Steam (`steamcmd +workshop_download_item ...`) instead of cloning this repo. Players still don't have to download anything because admins load it with `-serverMod=`.

## 1. Build a fresh PBO

```powershell
pwsh scripts/build.ps1 -Clean
```

Confirm the PBO landed at `@TakaroIntegration/Addons/TakaroIntegration.pbo`.

## 2. Open Publisher Tool

Steam → Library → Tools → DayZ Tools → Launch → "Publisher Tool".

## 3. Create a new Workshop entry

In Publisher Tool:

1. Click **Create New Mod**.
2. **Mod folder:** point at the absolute path to `@TakaroIntegration/` in this repo. Publisher Tool reads `mod.cpp` for the display name, description, and link.
3. **Visibility:** start with **Hidden** while you test, switch to **Public** when ready.
4. **Tags:** `Server-Side`, `Mod`, `Other`. Avoid client-facing tags so people don't try to load it client-side.
5. **Description:** copy the README's intro paragraph. Add a clear sentence: *"This is a server-side mod. Load with `-serverMod=`. Players do not need to download or subscribe."*
6. **Preview image:** optional. Drop a 512×512 PNG at `@TakaroIntegration/gui/icon.paa` if you want one (convert with TexView2 from DayZ Tools).
7. Click **Upload**.

Publisher Tool will:
- Pack the PBO and supporting files into a Workshop bundle.
- Upload to Steam.
- Write the Workshop ID into `@TakaroIntegration/meta.cpp` (replacing `publishedid = 0`).

Once Steam confirms, the Workshop URL is `https://steamcommunity.com/sharedfiles/filedetails/?id=<publishedid>`.

## 4. Updating the mod

1. `pwsh scripts/build.ps1 -Clean`
2. Open Publisher Tool, select the existing mod, click **Update**.
3. Bump `version` in `src/takaro_integration/config.cpp` and `TakaroBridge.VERSION` so admins can see what they're running.
4. Add a release note in the Workshop changelog.

## 5. How admins consume it

```bash
steamcmd +force_install_dir C:\gameservers\dayz +login anonymous \
    +workshop_download_item 221100 <publishedid> +quit
```

After download, copy the contents of `steamapps/workshop/content/221100/<publishedid>/` to `@TakaroIntegration/` next to the server, and launch with `-serverMod="@TakaroIntegration"`.

## A note on signing

The mod does **not** need a `.bikey` because it's loaded with `-serverMod=` and clients never see it. If you want to sign it anyway (some hosts require all PBOs to be signed):

```powershell
pwsh scripts/build.ps1 -Sign 'C:\path\to\mykey.biprivatekey'
```

Place the matching `.bikey` in the server's `keys/` directory.
