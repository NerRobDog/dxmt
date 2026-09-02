# Instructions for dummies — 5 minutes, no prior knowledge needed

You need: a Mac with Apple Silicon, CrossOver 26.x with Overwatch 2 already
installed and working in a bottle. That's it.

## Install

1. Download `dxmt-ow2-pack-vX.Y.tar.gz` from the
   [latest release](../../../../releases) and double-click it in Downloads —
   you get a `dxmt-ow2-pack` folder.

2. **Quit CrossOver completely** — the game, Battle.net, everything.
   (CrossOver menu → Quit CrossOver.)

3. Open **Terminal** (Cmd+Space, type "Terminal", Enter) and paste:

   ```
   cd ~/Downloads/dxmt-ow2-pack
   bash install.sh
   ```

4. The installer will:
   - back up your original CrossOver files (so you can always undo),
   - install the patched DXMT,
   - put a ready-made config at `~/dxmt.conf`,
   - **ask which bottle to set up** — type the number of the bottle with
     Overwatch 2 (usually the Battle.net one) and press Enter. It edits the
     bottle config for you (with a backup).

5. Start Battle.net again, launch Overwatch 2. In the game's video settings:
   - Graphics API / renderer: **DirectX 11** (if the option exists)
   - FPS limit: set it high (120 or 300) — the pacing to a flat 60 comes
     from the DXMT config, not the in-game limiter.

Done.

## What to expect

- Memory usage drops massively on 16 GB Macs (this was the point).
- The first few sessions will have brief hitches: every new hero, skin,
  ability effect and map compiles its shaders once, then they are cached
  forever. **The more you play, the smoother it gets.** After a few days of
  varied play it settles.

## Undo everything

```
cd ~/Downloads/dxmt-ow2-pack
bash uninstall.sh
```

## FAQ

- **CrossOver updated and the fix is gone.** Updates overwrite the files.
  Just run `bash install.sh` again.
- **`operation not permitted` when running the script.** Use `bash install.sh`
  (not `./install.sh`) — macOS quarantines downloaded files.
- **I want my old settings back.** Your bottle config backup is next to the
  original: `cxbottle.conf.bak-dxmt`; the stock DXMT libs are in
  `~/dxmt-stock-backup`.
- **120 fps?** On current Apple Silicon the GPU can't hold a stable 8.3 ms
  frame in big fights — flat 60 feels better than 70–90 with dips. That's
  why the config paces to 60.
