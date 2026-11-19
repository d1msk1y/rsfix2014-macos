# RocksmithFix

Fixes for **Rocksmith 2014 Remastered (Steam, macOS)** on macOS 15.4 and later:

1. **Crash on start.** The game dies instantly with `EXC_BAD_ACCESS at 0x7fff5fc01008`.
2. **Using a regular audio interface** instead of the Real Tone Cable, e.g. a Focusrite Clarett/Scarlett. Without the fix, a multi-input interface gives a constant loud hum and the game ignores the guitar.

It's one small library plus a launcher script. It doesn't modify any game or Steam files, and it does **not** remove or bypass the game's DRM: you still need to own the game on Steam.

> Tested on: Apple Silicon (M1), macOS 15.8.1, Steam client Nov 2026, Clarett 4Pre Thunderbolt.

---

## Install

### Option A: prebuilt zip

1. Download `RocksmithFix-<version>.zip` from the Releases page and unzip it.
2. Move the `RocksmithFix` folder into your **home folder**, e.g. `/Users/YOURNAME/RocksmithFix`.
   The path **must not contain spaces**: Steam splits launch options on spaces.
3. Remove the download quarantine (run once in Terminal):
   ```sh
   xattr -dr com.apple.quarantine ~/RocksmithFix
   ```

### Option B: build from source

Requires the Xcode Command Line Tools (`xcode-select --install`).

```sh
git clone <this repo> ~/RocksmithFix
cd ~/RocksmithFix
make
```

### Tell Steam to use it

1. Find your full path:
   ```sh
   echo ~/RocksmithFix/launch
   ```
2. In Steam: right-click **Rocksmith 2014** → **Properties** → **General** → **Launch Options**, and paste:
   ```
   /Users/YOURNAME/RocksmithFix/launch %command%
   ```
   Use the path printed in step 1. Steam does not understand `~`, and it does not support the `VAR=value %command%` form on Mac (that gives *"OS Error 260"*).

Launch the game from Steam as usual.

---

## Audio interface setup (optional)

Skip this section if you use the original Real Tone Cable.

The game only accepts a device named like the Real Tone Cable. macOS can create a virtual "Aggregate Device" around your interface and give it that name:

1. Open **Audio MIDI Setup** (Applications → Utilities).
2. Click **+** (bottom left) → **Create Aggregate Device**.
3. Tick **Use** next to your interface, and set the sample rate to **48 kHz**.
4. Rename the device to exactly: **`Rocksmith USB Guitar Adapter`**
5. Plug the guitar into **input 1** with **INST / Hi-Z** on. Start with the gain low and raise it until notes register cleanly.
6. Keep your interface (or anything else) as the normal sound **output**.

`Rocksmith.ini` in the game folder must contain `RealToneCableOnly=0`, which is the default.

**Guitar on a different input?** Edit `launch` and change `RS_INPUT_CHANNEL` (1 = first input, 2 = second, …).

---

## Troubleshooting

Every launch writes `dyldfix.log` next to `launch`. A healthy log looks like:

```
patching .../steamloader.dylib __dyld at 0x...
lookup __dyld_lookup_and_bind -> 0x...
audio: target device 121, 20 input channels in 1 stream(s); feeding game channel 1
audio: game opened input IOProc on target (...) = 0; wrapping to 1 channel
audio: AudioDeviceStart(121,...) = 0
audio: input is flowing (1 buffer(s))
```

| Symptom | Cause / fix |
|---|---|
| Steam says **OS Error 260** | Launch options are wrong. Use `/full/path/launch %command%` exactly, with no `VAR=` prefix and no spaces in the path. |
| Game still crashes on start, no `patching` line in log | The library didn't load. Check the launch-options path and re-run the `xattr` command. |
| Log says `steamloader.dylib not found` | Steam isn't in the default location (`~/Library/Application Support/Steam`). |
| `audio: no device named ...` | Aggregate device missing or misnamed. The name must match exactly. |
| Constant hum, game ignores guitar | Old build, or the fix isn't loaded. Check that the log has the `audio:` lines. |
| "Current input: none" | Check for `AudioDeviceStart(<id>,...) = 0` in the log; anything other than `= 0` is a bug, so please report it with the log. |
| Hum/noise that changes with the guitar | Gain too high. Turn the interface's input gain down. |

Rocksmith on Mac can occasionally crash mid-session in its own rendering code (seen once after about 80 minutes of play). That's a game bug unrelated to this fix.

---

## Uninstall

Clear the Steam launch options, delete the `RocksmithFix` folder, and, if you made one, delete the aggregate device in Audio MIDI Setup.

---

## How it works

Short version below; see [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) for the full write-up.

- **Crash:** Steam injects `steamloader.dylib` (last updated 2021) into the game to start its DRM. That loader has a legacy `__DATA,__dyld` section, which old dyld filled in at load time with the address of `dyld_func_lookup`. Since macOS 15.4 dyld no longer does this, so the slot keeps its 2010-era placeholder `0x7fff5fc01008`, and the game jumps there and crashes. `rs_dyldfix.dylib` loads **before** steamloader and fills the slot with a working `dyld_func_lookup`, implemented with `dlsym`.
- **Audio:** the library intercepts the game's CoreAudio calls for the `Rocksmith USB Guitar Adapter` device. It reports 1 input channel instead of N, and hands the game only the selected channel from each audio buffer. It also handles the game's legacy habit of passing its callback function pointer to `AudioDeviceStart`/`Stop` instead of the ID it was given.

## Limitations

- macOS + Steam version of the game only; it's Intel-only and runs through Rosetta 2. Apple plans to phase Rosetta out after macOS 27, which would affect the game itself.
- Assumes Steam is installed in its default location.
- Rocksmith may still crash occasionally in its own code (see Troubleshooting).
