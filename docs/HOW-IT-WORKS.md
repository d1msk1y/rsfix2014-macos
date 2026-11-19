# How it works

These notes cover how the two problems were diagnosed and how each fix works.

## 1. Crash on start (macOS 15.4+)

### Symptom

The game dies before showing a window. The crash report shows:

```
EXC_BAD_ACCESS (SIGSEGV)  KERN_INVALID_ADDRESS at 0x00007fff5fc01008
Thread 0 (main):
  ??? (translated code)
  ??? (translated code)
  steamloader.dylib  CGlobalInitter::CGlobalInitter() + 246
  dyld               dyld4::Loader::findAndRunAllInitializers(...)
```

`0x7fff5fc00000` was dyld's fixed load address on old macOS versions, so the game is jumping into a dyld that is no longer there.

### Cause

Steam launches Mac games with `DYLD_INSERT_LIBRARIES` pointing at `steamloader.dylib` (in `Steam.AppBundle/Steam/Contents/MacOS/`). Its static initializer, `CGlobalInitter`, bootstraps the game's Steam DRM:

1. `CrackMainImage()` finds a stub header at the end of the game's `__TEXT` (magic `'etan'`), decrypts it, and writes the address of steamloader's own `__dyld_func_lookup` into that header.
2. It then calls the decrypted stub, which calls `__dyld_func_lookup` to resolve the functions it needs.

steamloader's `__dyld_func_lookup` is a single instruction, `jmp *[__DATA,__dyld + 8]`. The `__DATA,__dyld` section is a legacy mechanism from old `crt1.o`/`dylib1.o`. On disk it contains placeholders:

```
__DATA,__dyld:  00 10 c0 5f ff 7f 00 00   08 10 c0 5f ff 7f 00 00
                 = 0x7fff5fc01000           = 0x7fff5fc01008
                 (lazy binder)               (dyld_func_lookup)
```

Old dyld overwrote these placeholders with real addresses at load time. dyld in macOS 15.4+ no longer does, so the jump goes to `0x7fff5fc01008`, which isn't mapped, and the game crashes. `dyld_func_lookup` is also no longer exported anywhere, so nothing else can be pointed at.

### Fix (`src/dyldfix.c`)

`rs_dyldfix.dylib` is placed **first** in `DYLD_INSERT_LIBRARIES`. All inserted images are mapped before any initializer runs, and inserted images initialize in list order, so its constructor runs before steamloader's. It:

1. Finds `steamloader.dylib` among the loaded images.
2. Locates its `__DATA,__dyld` section with `getsectiondata()`.
3. Writes:
   - slot 0: a lazy binder that logs and aborts (never called in practice);
   - slot 1: `fixed_dyld_func_lookup(name, &addr)`, which maps old dyld function names onto `dlsym`:
     `__dyld_X` → `_dyld_X`, else `X` (for `__dyld_dlopen` → `dlopen`, etc.).

In practice the DRM stub asks for exactly one function, `__dyld_lookup_and_bind`, which `libdyld` still exports, and everything continues normally. The DRM itself runs unchanged.

### Why a launcher script

Steam on Mac does not support `VAR=value %command%` in launch options. It treats the first word as the program to execute, which gives *OS Error 260 (file not found)*. So the launch option is `/path/launch %command%`. The script receives the `.app` path, sets `DYLD_INSERT_LIBRARIES` to `rs_dyldfix.dylib:steamloader.dylib:gameoverlayrenderer.dylib`, and `exec`s the game binary. `DYLD_*` variables would be stripped when passing through SIP-protected `/bin/sh` anyway, so the script builds the list itself.

## 2. Audio interfaces instead of the Real Tone Cable

### Symptom

With an Aggregate Device named `Rocksmith USB Guitar Adapter` wrapping, for example, a Clarett 4Pre, the game produces a loud constant hum that doesn't react to the guitar at all, even when the guitar is unplugged.

### Cause

The Real Tone Cable is a 1-channel input device. An aggregate of a multi-channel interface exposes every channel. The Clarett 4Pre has 20: 8 analog, 2 S/PDIF, 2 loopback and 8 ADAT, in one interleaved float32 stream. The game reads that buffer as if it were mono, so it receives a scrambled mix of all channels, which comes out as a tone.

An aggregate device can't expose fewer channels than its sub-device has, so this can't be fixed in Audio MIDI Setup.

### How the game uses CoreAudio

Tracing showed that the game:

- enumerates **all** input devices and, for each, reads `'slay'` (StreamConfiguration), `'sfmt'`/`'pft '` (formats), `'ltnc'` (latency) and `'saft'` (safety offset);
- opens a raw HAL IOProc on each with `AudioDeviceCreateIOProcID`;
- starts and stops them with `AudioDeviceStart(dev, <its own IOProc function pointer>)`, a legacy pattern, instead of the `AudioDeviceIOProcID` it got back. CoreAudio accepts this as long as the registered proc is that function.

### Fix (`src/audiofix.c`)

The library interposes CoreAudio through the `__DATA,__interpose` section, but only for the device named `Rocksmith USB Guitar Adapter`:

| Call | What it does |
|---|---|
| `AudioObjectGetPropertyData[Size]` | Input-scope `StreamConfiguration` → 1 buffer × 1 channel. Stream formats (`sfmt`, `pft `, virtual/physical, and lists) → `mChannelsPerFrame = 1`, bytes-per-frame adjusted. |
| `AudioObjectSetPropertyData` | If the game sets a 1-channel format, only the sample rate is applied to the real device. |
| `AudioDeviceCreateIOProcID` | Registers a wrapper IOProc. Each callback copies channel `RS_INPUT_CHANNEL` out of the interleaved buffer into a mono buffer and calls the game's proc with it. |
| `AudioDeviceStart` / `Stop` / `DestroyIOProcID` | Translates the game's function-pointer "ID" into the wrapper's real ID. Without this, `AudioDeviceStart` fails with `'nope'` and the game shows "current input: none". |

All other devices and calls pass through untouched. The audio callback makes no allocations and takes no locks.

## Diagnosing further problems

- `dyldfix.log` (next to `launch`) is rewritten on every launch.
- Crash reports: `~/Library/Logs/DiagnosticReports/Rocksmith2014-*.ips`. On Apple Silicon these can take a minute or more to appear after a crash, while ReportCrash processes the Rosetta process.
- System log (use the full path; in zsh, `log` is a different built-in command):
  ```sh
  /usr/bin/log show --last 10m --predicate 'process == "Rocksmith2014"'
  ```
