# Test runs

[← Technical guide](TECHNICAL.md) · [Contributing](../CONTRIBUTING.md)

These switches make a scripted run of the game quicker and less dependent on the PC it runs on. They are test aids, not settings for playing. Each one is off unless its environment variable is set, and with it unset the game runs exactly as before. They were ported from ssbu-recomp (`ssbu-recomp/tests/DEBUG_MODE.md`). WindowSmash builds game version 13.0.5 only.

| Variable | What it does |
| :--- | :--- |
| `SSBU_SPEED=<n>` | Fast-forward: one display refresh per frame shown, at n × 60 a second; `0` is uncapped. `1` or unset is normal pacing. |
| `SSBU_SEQ=<steps>` | Player 1's input as a sequence counted in the game's frames, not in seconds. |
| `SSBU_SEQ_LOG=<n>` | With `SSBU_SEQ`: logs the draw calls of the last frame shown every n polls, to find the ranges for `@draws`. |
| `SSBU_SEQ_KEYBOARD=1` | With `SSBU_SEQ`: the keyboard still plays. It is ignored otherwise. |
| `SSBU_SKIP_MOVIES=<n>` | Every movie's stream ends after n samples (`1` means 12), and the game ends the movie as it ends any movie. |

`SSBU_INPUT` (buttons on a timetable in seconds) is unchanged, and it still adds to player 1 when `SSBU_SEQ` is set.

## `SSBU_SPEED`

Normally the vsync thread gives the game 60 refreshes a second whether or not a frame is ready ([`ipc_vi.c`](../runtime/ipc_vi.c)). The game counts those refreshes and runs extra logic frames when it sees it has fallen behind, so a slow PC plays a slightly different game than a fast one.

With `n` other than 1 there is no refresh clock:

- **One refresh per frame shown.** The vsync thread waits for the frame the game queues, shows it and then gives exactly one refresh. The game steps one frame per frame shown, however fast or slow the PC is.
- **The pace is n × 60 frames a second at most** (`0`: as fast as the game draws, which can be slower than 60). After a stall such as a load, there is no burst to catch up.
- **Idle refreshes.** When the game has queued nothing for 100 ms, one refresh is given anyway, in case it is waiting for one. These are counted in the log.
- **Frames are shown at once.** They are not held to the monitor's refresh: Direct3D gets the tearing flag where the swap chain allows it, and macOS turns off the display sync.
- **A line a second:** `[display] speed: 240 frames shown in the last 1.00 s (240 a second), 0 idle refreshes`.

What it does not guarantee: the game counts refreshes on a thread of its own and compares the count at the end of each frame. If that thread runs late, one frame can still see two refreshes and run two logic frames. ssbu-recomp waited until the game had counted each refresh; this runtime does not read that count yet. The deterministic mode, still to be ported, removes those extra steps.

**Audio is not touched.** The sound device still sets the pace of the game's mixer ([`ipc_audio.c`](../runtime/ipc_audio.c)), so the sound does not speed up with the picture. WindowSmash has no switch to mute it (`SSBU_AUDIO` only picks the Windows output; ssbu-recomp's scripts used an `SSBU_AUDIO_MUTE` that WindowSmash doesn't have). Turn the volume down.

## `SSBU_SEQ`

The game reads player 1's controller once per frame. Each read (a *poll*) moves the sequence on by one *tick*. A hook can change that: `input_seq_set_steps` in [`input_seq.h`](../runtime/input_seq.h) is what the deterministic mode's port will use to count fight frames instead.

Steps are separated by spaces or new lines, and `#` starts a comment that runs to the end of the line. Keys are the names `SSBU_INPUT` takes, joined by `+`: `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK`, and `SLEFT SUP SRIGHT SDOWN` for the left stick.

| Step | Meaning |
| :--- | :--- |
| `<n>` | Nothing for n ticks. |
| `<n>:<keys>[/<h>]` | Nothing for n ticks, then the keys for h ticks (8 if not given). |
| `@<mark>` | Wait until the mark has been raised (marks are listed below). |
| `@draws:<lo>-<hi>[/<n>]` | Wait until every frame shown has had lo to hi draw calls for n ticks in a row (20 if not given). A screen can be recognised by its draw count, whatever the loading time was. |
| `@until:<lo>-<hi>[/<n>]:<keys>[/<p>]` | Press the keys for 8 ticks, at once and again every p ticks (200 if not given), until the draw count has been in range for n ticks in a row. A press under way is finished first. |
| `@untilmark:<mark>:<keys>[/<p>[/<h>]]` | The same, until the mark has been raised, with presses of h ticks. |

What it guarantees:

- **Exact counts.** A wait step ends on the tick its condition is met, and the next step starts on the tick after. Holds are exact: `0:A/3 2:B` is A on ticks 1-3, nothing on 4-5, then B on 6-13.
- **Player 1 is the sequence's.** Player 1's pad is not read, and player 1 is connected as a Pro Controller throughout. The keyboard plays for nobody unless `SSBU_SEQ_KEYBOARD=1`. Players 2-8 keep their pads, so unplug them for a run that has to repeat exactly.
- **Marks:** `movie` when a movie's stream is opened, and `module:<name>` when the game has loaded that module (the name its `[ro] loaded` log line gives). The fight mark comes with the later ports.
- **Draw counts** are the draw calls the GPU ran for each frame shown. They are not the same numbers as ssbu-recomp's, which counted at a higher level, so its ranges don't carry over: use `SSBU_SEQ_LOG` to find yours. With `SSBU_GPU_THREAD=0` they are approximate.
- **A sequence that doesn't parse stops the game at start**, naming the step and what is wrong with it.
- **The log** has one line per press and per wait that ends: `[seq] poll 812: tick 812, step 3 (60:A): keys for 8 ticks`.

## `SSBU_SKIP_MOVIES`

After n samples (12 when n is 1) a movie's stream reports that it has no more data, and the game ends the movie through its normal end-of-stream path. ssbu-recomp found that an end before any sample is not one the game gets through, which is why the default is 12 and not 0. The count starts again when the game seeks the stream: it does that when a movie started late, and samples it read before the seek are no use to it.

It works on the movie library's `movie::Extractor` vtable in `subsdk0` ([`hle_movie_skip.c`](../runtime/hle_movie_skip.c)). If that vtable is not as expected, the log says so and movies play as usual.

A known gap: ssbu-recomp's own decoders took inputs again when the game seeked a stream that had already ended early. Here the decoders are the game's own and can't do that, so such a movie may wait forever. The log says when that seek happens. A larger count, such as 120, lets the movie start before its stream ends.

## An example run

A made-up sequence for a run (its draw counts are made up too, so find yours with `SSBU_SEQ_LOG`):

<!-- example sequence: tests/test_test_runs.py parses it -->
```text
# wait for the opening movie, then press A until the next screen holds still
@movie
@until:40-60:A/120
# a second's pause, then into the menu
60:A
@draws:100-300/30
30:DOWN/4 10:A
# watch for ten seconds
600
```

Saved as `run.seq`, it runs at four times normal speed with the movies cut short:

```sh
SSBU_SPEED=4 SSBU_SKIP_MOVIES=1 SSBU_SEQ="$(cat run.seq)" SSBU_SEQ_LOG=60 ./build/ssbu.exe dump/modules > run.log 2>&1
```

The log then has the `[seq]` lines of each step, the `[display] speed:` line every second and the `[movie]` line of each movie's end.

## For the ports that follow

[`game_addr.h`](../runtime/game_addr.h) has the game addresses that the fight hash, the deterministic mode and the debug match use, for 13.0.5. Each has the 13.0.4 address it came from beside it, and `tests/test_game_addr.py` checks them against ssbu-recomp's table. [`input_seq.h`](../runtime/input_seq.h) has the steps hook and the marks. [`tools/dev/testruntest.c`](../tools/dev/testruntest.c) checks the parser, the step machine, the arithmetic of the speed and the movie count, and the address mapping.
