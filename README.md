<p align="center"><img src="ps5/5s2cd/sce_sys/icon0.png" width="160" alt="5S2CD icon"></p>

<h1 align="center">5S2CD</h1>

<p align="center"><b>Play your own PS2 game discs on a jailbroken PS5.</b><br>
Put the disc in, pick it, play. No ripping, no copying files.</p>

---

5S2CD is the PS2 emulator **[PS5SX2](https://github.com/Swordpdf/PS5SX2)** by **swordpdf**, with more additions:
it reads **real PS2 discs** from the PS5's own drive or a **USB DVD drive**, gives each inserted disc **its own tile on
the PS5 home screen**, can **copy a disc** to a file, and adds **favorites** and **shelves** to sort your games. Your PS5SX2 saves, memory cards and
settings work in it too.

## What's new

| | Feature | What it does |
| :---: | --- | --- |
| 💿 | **PS5 disc drive reads PS2 games** | Put a PS2 DVD (silver underside) in your PS5's own drive. 5S2CD shows it first on the shelf with its name and cover. Press **X** to play. |
| 🔌 | **External USB DVD drive** | Plug a USB DVD drive into the PS5 and use it the same way. It also reads **PS2 CDs (blue underside)**, which the PS5's own drive can't read, and it works on the Digital Edition. Both drives work at the same time. |
| ⚡ | **Insert the disc any time** | Before you open 5S2CD: it's on the shelf. While 5S2CD's shelf is open: a disc in the **USB drive** shows up on the shelf within a couple of seconds; a disc in the **PS5's own drive** closes 5S2CD (the PS5 goes to its home screen for a disc anyway), so the game's home-screen tile appears right away. |
| 🏠 | **The game's own tile on the PS5 home screen** | Insert a disc and a tile with the game's name and cover appears on the PS5 home screen. Open it and the game starts straight from the disc. Take the disc out and the tile goes away. *(Needs the included `discwatch.elf`.)* |
| 💾 | **Copy this disc** | Select a drive's tile, press **Square** and pick where the copy goes: the USB drive with your PS2 games, another USB drive, or the PS5's internal storage (**X** twice). The progress shows on the shelf; afterwards the game plays without the disc. |
| ⭐ | **Favorites** | Press **L3** on any game to make it a favorite. Favorites come first on the shelf, with a badge. |
| 🗂️ | **Shelves** | Press **Circle** to switch the shelf: **All**, **Disc drives**, **Favorites** or **Disc images**. |
| 🖼️ | **Covers for discs** | Each disc gets its box art automatically, like your disc images do. |
| 🎨 | **Its own look** | A darker shelf, and its own icon and home-screen background. |

Everything else is PS5SX2 as you know it: your disc images (ISO, CHD, CSO...), settings, memory cards, save states
and the in-game menu.

> **Good to know:** the PS5 always says **"Unsupported Disc"** when you insert a PS2 disc. That's the console's own
> message and it's harmless: choose *Go to Home* and open 5S2CD (or the game's tile).
>
> **Limits:** the PS5's own drive loads a little slower than a USB drive, and going *back to the menu* restarts the
> app.

<sub>Based on PS5SX2 vk-285-139. The original PS5SX2 readme is kept as [README-PS5SX2.md](README-PS5SX2.md).</sub>

## Requirements

- A **jailbroken PS5** with a payload loader (`elfldr` on port 9021) and a HEN that can let apps out of their
  sandbox (the same setup PS5SX2 needs). Tested on firmware **13.00**.
- **[PS5SX2](https://github.com/Swordpdf/PS5SX2/releases)** installed and working (with the BIOS dumped from your own
  console) - 5S2CD uses its system module and its `/data/PCSX2` folder.
- **ShadowMountPlus** to register the app (and the disc tiles).
- A PS5 with a **disc drive**, or a **USB DVD drive**.

## Install

1. Download **`5S2CD-v1.1.zip`** from the [Releases](../../releases) page and unzip it.
2. With a file manager on the PS5, copy the zip's `PPSA99213` folder to `/data/homebrew/` (it holds `eboot.bin` and
   `sce_sys` with 5S2CD's name, icon and background).
3. Copy the `sce_module` folder of PS5SX2 (`/data/homebrew/PPSA99203/sce_module`) into `/data/homebrew/PPSA99213/`.
4. Copy `relaunch.elf` to `/data/PCSX2/relaunch.elf`.
5. Allow `PPSA99213` out of the sandbox wherever you allowed PS5SX2 (`PPSA99203`), for example your HEN's app list or
   `/data/whitelist.txt`.
6. Let ShadowMountPlus pick up the app (rescan, or restart the PS5). **5S2CD** appears on the home screen.
7. *Optional:* for the home-screen disc tiles, add `discwatch.elf` to your payload loader's **autoload** list, so it
   starts again after every jailbreak (payloads don't survive a restart). You can also send it by hand once.
   *Nicer tile art (optional):* get a free API key from [SteamGridDB](https://www.steamgriddb.com/profile/preferences/api)
   and save it in a text file on the PS5 at `/data/PCSX2/disc-tiles/steamgriddb-key.txt`. The tiles then also use
   SteamGridDB's backgrounds and logos. Your key stays on your console; without it the tiles use the disc's cover.

## Use

Put a PS2 disc in, open **5S2CD**: the drive is the first tile on the shelf, with the game's name and cover. Press
**X** to play. Everything else - settings, memory cards, save states, the in-game menu - works as in PS5SX2.

- **"Unsupported Disc" is normal.** The PS5 says this for every PS2 disc - it's the console's own message and nothing
  can change it. Choose *Go to Home* and open 5S2CD (or the disc's own tile, see below): the game is there.
- **Silver or blue?** Silver-underside PS2 discs are DVDs: they work in the PS5's own drive and in a USB DVD drive.
  Blue-underside PS2 discs are CDs: the PS5's own drive can't read CDs at all, so use a USB DVD drive for them.
- **Disc in while 5S2CD is open:** in the USB drive it shows up on the shelf within a couple of seconds. In the PS5's
  own drive, 5S2CD closes (not during a game or a disc copy) so the disc's tile appears on the home screen at once.
- **Home-screen tile for the disc** (open the game straight from the home screen) needs `discwatch.elf` running,
  which is why it belongs in autoload. A disc that was already in when it starts gets its tile too.
  ShadowMountPlus adds the tile only while **no game or app is running**: if a game is open, the tile appears once
  you close it (*Options -> Close Game* on its tile).
- **Copy this disc:** select the drive's tile, press **Square**, choose a place, press **X** twice. While it copies, no
  game starts; to cancel, open the Square menu again (*Stop the copy*). The copy is on the shelf the next time 5S2CD
  opens, as `<Title> (<serial>).iso`.
- After a restart, **jailbreak first**: 5S2CD (like PS5SX2) can only read the drive once your HEN is running.

## Build from source

Linux (Ubuntu 22.04 or WSL), clang/lld 18, the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) and
PS5SX2's public RADV dependencies (see `ps5/docs/build-dependencies.md`):

```sh
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk ps5/5s2cd/tools/build-deps.sh   # once
ps5/5s2cd/tools/build-5s2cd.sh                                           # -> ps5/build-radv/app/eboot.bin
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk ps5/5s2cd/relaunch/build.sh      # -> relaunch.elf
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk ps5/5s2cd/discwatch/build.sh     # -> build/discwatch.elf
```

What changed against PS5SX2:

| Part | Files |
| --- | --- |
| Disc reader (FreeBSD `cd(4)`: DVD layout, CD TOC, 2048-byte reads, read-ahead) | `ps5/coreorbis/orbis-shims/ProsperoDisc.cpp`, `ProsperoCDVD.cpp` |
| Drives on the shelf, favourites, tabs, covers, darker background, Copy this disc, a new disc closing the shelf | `ps5/frontend/fe_app.*`, `fe_games.*`, `fe_options.cpp`, `fe_covers.cpp`, `fe_ps5.cpp` |
| Disc boot, disc tiles, back to the menu via `relaunch.elf`, title ID `PPSA99213` | `ps5/coreorbis/main-boot.cpp`, `ProsperoHenJailbreak.cpp` |
| App identity (name, icon, background) | `ps5/5s2cd/sce_sys/` |
| Home-screen disc tiles | `ps5/5s2cd/discwatch/` |
| Relaunch helper | `ps5/5s2cd/relaunch/` |

## Credits

- **[PS5SX2](https://github.com/Swordpdf/PS5SX2)** by **swordpdf** - the PS5 port this is built on. Please support
  and star the original.
- **[PCSX2](https://github.com/PCSX2/pcsx2)** and its team - the emulator; the disc reader follows PCSX2's own
  FreeBSD/macOS `IOCtlSrc`.
- **[xlenore/ps2-covers](https://github.com/xlenore/ps2-covers)** - the covers.
- **ShadowMountPlus** - app and tile registration on the home screen.
- **[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)**, **[stb_image](https://github.com/nothings/stb)**,
  **[Mbed TLS](https://github.com/Mbed-TLS/mbedtls)**.

Made with the help of an AI assistant (Claude by Anthropic).

## License

GPL-3.0 like PCSX2 and PS5SX2 (see [COPYING.GPLv3](COPYING.GPLv3)).

*5S2CD is an unofficial homebrew project, not affiliated with or endorsed by Sony Interactive Entertainment or the
PCSX2 project. No BIOS, system files or games are included: use your own console's BIOS and discs you own.
"PlayStation", "PS2" and "PS5" are trademarks of Sony Interactive Entertainment Inc. Use at your own risk.*
