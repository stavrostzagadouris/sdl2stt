# sdl2stt

NOTE: this is totally a Claude vibe code for myself. I don't expect others to use it. It pairs with a endpoint to host the voice models.
The rest of the readme is all my good buddy Claude:


Push-to-talk voice input for Wayland. Hold a key, speak, release — the transcript is typed
into whatever field has focus.

Two pieces:

| | |
|---|---|
| `talkwave.c` | The recorder. libpulse capture on a dedicated thread, plus a small SDL2 level-meter bar pinned bottom-centre. On SIGUSR1 it writes a finalized WAV and switches the bar to a "processing" sweep; on SIGTERM it exits. Knows nothing about transcription. |
| `talk` | The orchestration. `start` / `stop` / `toggle`, PID state, a noise gate, the POST to your STT server, and `wtype` to inject the text. This is what your keybind calls. |

Speech-to-text is delegated, not bundled. `talk` POSTs a WAV to any server exposing an
OpenAI-compatible `POST /v1/audio/transcriptions` endpoint and types back whatever `text`
comes out, so there is no model to install here and nothing goes to a cloud provider unless
you point it at one.

### Designed as a cleanTTS companion

It was built against **cleanTTS**, and that is the intended pairing. cleanTTS serves an
OpenAI-compatible audio API — `/v1/audio/transcriptions` for STT alongside
`/v1/audio/speech` for TTS — and on Apple silicon it runs Parakeet on `mps`, which is fast
enough that a few seconds of speech comes back effectively instantly over a LAN or tailnet.
Sending no `model` parameter (which is what `talk` does) gets the server's default STT
engine, so no configuration beyond the URL is needed. The indicator bar's colours are
matched to the cleanTTS palette.

Nothing here is cleanTTS-specific, though — any server with that endpoint will do.

## Requirements

- Wayland compositor (developed on Hyprland)
- PulseAudio or PipeWire's pulse shim
- `sdl2` (or `sdl2-compat`), `libpulse`, `wtype`, `curl`, `python3`
- `libnotify` for failure notifications; `tailscale` optional, only for failure diagnosis

On Arch:

```
sudo pacman -S --needed sdl2-compat libpulse wtype curl python libnotify
```

## Build

```
gcc -O2 -o talkwave talkwave.c \
    $(pkg-config --cflags --libs sdl2) \
    $(pkg-config --cflags --libs libpulse-simple)
install -Dm755 talkwave talk -t ~/.local/bin/
```

Install both together. `talk` and `talkwave` talk to each other with signals, and a new
`talk` paired with an old `talkwave` binary (or the reverse) will lose recordings.

### Updating

`talkwave` is a compiled binary, so pulling new source is not enough. After every
`git pull`, rebuild and reinstall both files:

```
git pull
gcc -O2 -o talkwave talkwave.c \
    $(pkg-config --cflags --libs sdl2) \
    $(pkg-config --cflags --libs libpulse-simple)
install -Dm755 talkwave talk -t ~/.local/bin/
```

Nothing needs restarting: there is no background process, and the next key press uses the
new files.

Note `libpulse-simple` is a **separate** pkg-config module from `libpulse`; `pa_simple_*`
will not link without it.

## Configure

```sh
export TALK_STT_URL=http://10.0.0.5:5000/v1/audio/transcriptions
export TALK_STT_PEER=my-stt-host   # optional, Tailscale peer name for diagnostics
export TALK_GATE=0.02              # optional, noise-gate RMS floor
```

Defaults to `http://127.0.0.1:5000/v1/audio/transcriptions`.

Your compositor probably does **not** put `~/.local/bin` on the PATH of the processes it
spawns, so export these somewhere the session actually reads, or use absolute paths in the
bind.

## Bind it

### Ending the hold: do not use the release bind

The obvious wiring — `bind` to start, `bindr` on the same key spec to stop — **does not
work on Hyprland**, and it fails in a way that looks intermittent:

```
bind  = SUPER, SPACE, exec, talk start
bindr = SUPER, SPACE, exec, talk stop     # <-- silently never fires
```

A release bind carrying a modmask only matches while that modifier is *still physically
down*, so it fires only if you release SPACE before SUPER. Releasing SUPER first — the
natural way to end a hold — drops the keyup, `talk stop` never runs, and the recorder keeps
going until something else kills it.

Neither obvious workaround helps. Verified on Hyprland 0.56.2, with a bind that logged
every dispatch:

* A bare `SUPER_L` release bind fires on a *fresh* SUPER tap, but not on the SUPER keyup
  that ends a `SUPER+<key>` chord.
* A bare `SPACE` release bind with `ignore_mods` fires for ordinary typed spaces and goes
  **completely silent** for the whole duration of the chord — once the press bind consumes
  the press, the keyup is not offered to any other bind, in either release order.

So the hold is ended by **key repeat** instead. A repeat only matches while both keys are
still down, so it stops the instant either one lifts, whichever goes first:

```
bind  = SUPER, SPACE, exec, talk start    # + the `e` (repeat) flag -- see the Lua below
bindr = SUPER, SPACE, exec, talk stop super   # optional panic key

windowrulev2 = float, class:^(talkwave)$
windowrulev2 = pin,   class:^(talkwave)$
```

The repeat fires ~25x/s, so the handler must not fork a process each time. Keep it in the
compositor. Hyprland's Lua config:

```lua
local TALK_STATE = os.getenv("HOME") .. "/.cache/talk.state"
local TALK_BEAT  = os.getenv("HOME") .. "/.cache/talk.beat"

local function talk_press()
    local b = io.open(TALK_BEAT, "w")
    if b then b:write("1"); b:close() end
    local f = io.open(TALK_STATE, "r")
    if f then f:close(); return end     -- already recording: this was a repeat
    hl.exec_cmd("/home/you/.local/bin/talk start")
end

hl.bind("SUPER + SPACE", function() talk_press() end, { repeating = true })
```

`talk start` detaches a watchdog that deletes `talk.beat` every 200 ms; the first time it
does not reappear, the hold is over and the recording is transcribed. It also enforces a
2-minute cap, so a missed stop can never record indefinitely.

One consequence worth knowing: a very short tap records about a second, because the
watchdog waits out Hyprland's 600 ms `repeat_delay` before it starts looking for beats.

Use an absolute path: the compositor spawns children with a PATH that need not include
`~/.local/bin`.

SDL cannot place a borderless toplevel itself — Hyprland centres it and ignores the
requested x/y — so if you want the bar somewhere specific, move it with a rule. For a
1920-wide monitor, `(1920-280)/2 = 820` and 24px of clearance under the 48px bar:

```
windowrulev2 = move 820 100%-72, class:^(talkwave)$
```

`talk toggle` works too, if you prefer press-once-to-start, press-again-to-stop. It needs no
repeating bind — a plain `bind = SUPER, SPACE, exec, talk toggle` is enough — and only the
2-minute cap applies. Holding is the better default: a toggle lets recorder state drift out
of sync, and holding makes "am I recording?" self-evident.

`talk start` is push-to-talk only: without the repeating bind feeding it beats, the watchdog
ends the recording after about a second. Use `talk toggle` from scripts.

### What you see

While you hold, the bar is a level meter. When the hold ends the bar stays up and a group of
three bars sweeps side to side while the clip is gated and sent to the server. The bar is
taken down just before the text is typed, so a compositor that gave the bar keyboard focus
cannot swallow the transcript.

## Debugging

The compositor discards stderr, so everything goes to `~/.cache/talk.log` (trimmed to its
last 2000 lines once it passes about 1 MB):

```
tail -f ~/.cache/talk.log
```

Each stop logs the measured peak RMS and loud-window count, so you can see whether the
noise gate ate your clip before blaming the server.

Recordings live in `$XDG_RUNTIME_DIR` (falling back to `/tmp`) only until they have been
sent, and are written with mode 0600.

## Notes from building it

Every one of these failed *silently*, which is why they're worth writing down.

**PulseAudio's default record `fragsize` can be two seconds.** 64000 bytes at 16 kHz mono.
That delays first delivery and discards the in-flight partial fragment at stop — a 5-second
press yielded 4.0 seconds with the first word clipped. `talkwave` passes an explicit
`pa_buffer_attr` with a 40 ms `fragsize`. This was the actual cause; render-starvation and
ALSA-suspend were both wrong theories.

**Audio must never wait on pixels.** An earlier single-loop version interleaved
`pa_simple_read()` with rendering, so frame pacing throttled how fast audio drained. The
backlog queued server-side and was thrown away at SIGTERM, losing 45–70% of the recording.
Capture gets its own thread; the UI only peeks at a peak value under a mutex.

**Don't gate the first `SDL_RenderPresent` behind a blocking audio read** — the window never
maps, and there is no error.

**Writing a WAV header by seeking back over a stdio write stream garbles it.** Buffer the
PCM and write header-then-data once, at the end.

**`parec` may be a symlink to `pacat`** and never emit captured PCM to stdout. Don't build
on a `parec | ...` pipe; capture in-process.

**Mic input volume matters more than you'd think.** A laptop mic at 100% can sit ~40 dB over
its base gain and hard-clip everything — even room noise pins to full scale, which makes a
level gate impossible and makes the STT model invent words out of the mush. Check with
`pactl list sources | grep -m1 'Volume: front-left'`; around 40% was right here.

**A level gate cannot distinguish your voice from a TV.** Both are speech. The gate only
rejects near-silence — it needs at least three 20 ms windows above `TALK_GATE` or it discards
the clip and notifies rather than spending a request. It fails *open*: a measurement error
must never silently drop a real recording.

**Two things can end the same hold.** The watchdog and the panic bind can both fire for one
release. `talk stop` claims the recording by renaming the state file — atomic, so exactly
one of them proceeds — otherwise both transcribe the clip and the text is typed twice. For
the same reason each watchdog is tied to its recorder's PID, so one left over from an
earlier recording cannot end the next.

**Write struct fields with the size you declared.** The WAV header's `fmt ` chunk size was
a `uint16_t` written with `fwrite(&x, 4, 1, f)`, which emits two bytes of whatever sits next
to it on the stack. It happened to be zero in one build and garbage in another.

**`wait` doesn't work across shells.** `talk stop` can't `wait` on the recorder, because
`talk start` was a different process — the recorder isn't its child, so `wait` returns
immediately and races `curl` against a WAV that's still being written. `talkwave` writes
to `<file>.part` and renames it into place, so `talk stop` polls for a non-empty file.

## A note on exposure

`talk` sends your microphone audio to whatever `TALK_STT_URL` points at, in the clear over
plain HTTP, with no authentication. That is a deliberate fit for a private overlay network
(Tailscale, WireGuard) or a trusted LAN, where the transport is already encrypted and the
listener isn't reachable from anywhere else. Don't expose the STT server to the internet or
to an untrusted network on that basis — it has no auth of its own, and this is a microphone.

## License

MIT — see [LICENSE](LICENSE).
