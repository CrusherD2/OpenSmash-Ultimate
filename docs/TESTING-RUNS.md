# Test runs

[← Technical guide](TECHNICAL.md) · [Contributing](../CONTRIBUTING.md)

These switches make a scripted run of the game quicker and less dependent on the PC it runs on. They are test aids, not settings for playing. Each one is off unless its environment variable is set, and with it unset the game runs exactly as before. They were ported from ssbu-recomp (`ssbu-recomp/tests/DEBUG_MODE.md`). WindowSmash builds game version 13.0.5 only.

**Status: not yet run with the game.** The sequence parser, the step machine and the arithmetic of the speed and the movie count were tested offline on made-up input (`tests/test_test_runs.py`). The runtime files compile against the macOS shims. None of these switches has run with the game in WindowSmash, on any platform. Their Windows and Linux builds were not compiled either. This guide says what each switch is built to do. Where that rests on something ssbu-recomp measured, it says so. Each section ends with what a run still has to check.

| Variable | What it does |
| :--- | :--- |
| `SSBU_SPEED=<n>` | Fast-forward: one display refresh per frame shown, at n × 60 a second; `0` is uncapped. `1` or unset is normal pacing. |
| `SSBU_SEQ=<steps>` | Player 1's input as a sequence counted in the game's frames, not in seconds. Empty, or with no steps, it is the same as unset. |
| `SSBU_SEQ_LOG=<n>` | With `SSBU_SEQ`: logs the draw calls of the last frame shown every n polls, to find the ranges for `@draws`. |
| `SSBU_SEQ_KEYBOARD=1` | With `SSBU_SEQ`: the keyboard still plays. It is ignored otherwise. |
| `SSBU_SKIP_MOVIES=<n>` | Every movie's stream ends after n samples (`1` means 12), and the game ends the movie as it ends any movie. |

`SSBU_INPUT` (buttons on a timetable in seconds) is unchanged, and it still adds to player 1 when `SSBU_SEQ` is set.

## `SSBU_SPEED`

Normally the vsync thread gives the game 60 refreshes a second whether or not a frame is ready ([`ipc_vi.c`](../runtime/ipc_vi.c)). The game counts those refreshes and runs extra logic frames when it sees it has fallen behind, so a slow PC plays a slightly different game than a fast one.

With `n` other than 1 there is no refresh clock:

- **One refresh per frame shown.** The vsync thread waits for the frame the game queues, shows it and then gives exactly one refresh. The game should then step one frame per frame shown, however fast or slow the PC is. ssbu-recomp measured that at 4× and uncapped: 600 game steps per 600 frames shown. It also waited for the game to count each refresh, which this runtime does not do yet (see below).
- **No refresh without a frame.** While the game queues nothing (a load, a shader compile), it gets no refresh, however long that lasts. A refresh there would count as a dropped frame and run an extra logic frame. ssbu-recomp gave none either, and the game still booted to a fight at 2×, 4× and uncapped.
- **The pace is n × 60 frames a second at most** (`0`: as fast as the game draws, which can be slower than 60). After a stall such as a load, there is no burst to catch up.
- **Frames are shown at once.** They are not held to the monitor's refresh: Direct3D gets the tearing flag where the swap chain allows it, and macOS turns off the display sync.
- **The game's own waits in seconds don't speed up.** ssbu-recomp found that the movie's pre-roll and the loading threads take four times as many frames at 4×. Wait for screens with marks and `@draws`, not with counts of ticks.
- **A line a second:** `[display] speed: 240 frames shown in the last 1.00 s (240 a second)`. During a load it says 0 frames.

What it does not guarantee: the game counts refreshes on a thread of its own and compares the count at the end of each frame. If that thread runs late, one frame can still see two refreshes and run two logic frames. ssbu-recomp waited until the game had counted each refresh; this runtime does not read that count yet. The deterministic mode, still to be ported, removes those extra steps.

**Audio is not touched.** The sound device still sets the pace of the game's mixer ([`ipc_audio.c`](../runtime/ipc_audio.c)), so the sound does not speed up with the picture. WindowSmash has no switch to mute it (`SSBU_AUDIO` only picks the Windows output; ssbu-recomp's scripts used an `SSBU_AUDIO_MUTE` that WindowSmash doesn't have). Turn the volume down.

Not yet checked in a run:

- that boot reaches the menus and a fight at 4 and at 0 on Windows, macOS and Linux;
- that boot doesn't stall without refreshes. If the speed lines say 0 frames for good, the game is waiting for a refresh before it draws, which ssbu-recomp never saw: please report it;
- the rate in the speed lines;
- whether Direct3D without tearing support still runs faster than the monitor.

## `SSBU_SEQ`

ssbu-recomp found that the game reads player 1's controller once per frame (`ssbu-recomp/runtime/nnhid.c`). That has not been checked in WindowSmash yet. Each read (a *poll*) moves the sequence on by one *tick*. A hook can change that: `input_seq_set_steps` in [`input_seq.h`](../runtime/input_seq.h) is what the deterministic mode's port will use to count fight frames instead.

Steps are separated by spaces or new lines, and `#` starts a comment that runs to the end of the line. Keys are the names `SSBU_INPUT` takes, joined by `+`: `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK`, and `SLEFT SUP SRIGHT SDOWN` for the left stick.

| Step | Meaning |
| :--- | :--- |
| `<n>` | Nothing for n ticks. |
| `<n>:<keys>[/<h>]` | Nothing for n ticks, then the keys for h ticks (8 if not given). |
| `@<mark>` | Wait until the mark has been raised (marks are listed below). |
| `@draws:<lo>-<hi>[/<n>]` | Wait until every frame shown has had lo to hi draw calls for n ticks in a row (20 if not given). A screen can be recognised by its draw count, whatever the loading time was. |
| `@until:<lo>-<hi>[/<n>]:<keys>[/<p>]` | Press the keys for 8 ticks until the draw count has been in range for n ticks in a row. The first press comes at once and the next p ticks after it began (200 if not given), each on a tick when the count is out of range. A press under way is finished first. |
| `@untilmark:<mark>:<keys>[/<p>[/<h>]]` | Press the keys for h ticks (8 if not given), at once and again every p ticks (200 if not given), until the mark has been raised. A press under way is finished first. |

ssbu-recomp found that `@until` can race a movie cut short: a press that lands just as the next screen appears can pass that screen too. With `SSBU_SKIP_MOVIES`, wait for that screen with `@draws` instead, as the example below does.

What the step machine does (tested offline on made-up sequences):

- **Exact counts.** A wait step ends on the tick its condition is met, and the next step starts on the tick after. Holds are exact: `0:A/3 2:B` is A on ticks 1-3, nothing on 4-5, then B on 6-13.
- **A sequence that doesn't parse stops the game at start**, naming the step and what is wrong with it. An empty `SSBU_SEQ`, or one with nothing but spaces and comments, is the same as unset: the log says so and player 1 plays as usual.

What the runtime does around it (not yet run with the game):

- **Player 1 is the sequence's.** Player 1's pad is not read, and player 1 is connected as a Pro Controller throughout. The keyboard plays for nobody unless `SSBU_SEQ_KEYBOARD=1`. Players 2-8 keep their pads, so unplug them for a run that has to repeat exactly.
- **Marks:** `movie` when a movie's stream is opened, and `module:<name>` when the game has loaded that module (the name its `[ro] loaded` log line gives). The fight mark comes with the later ports. Up to 512 different marks are kept; past that the log says a mark was not kept, and a step waiting for it waits forever.
- **Draw counts** are the draw calls the GPU ran for each frame shown. They are not the same numbers as ssbu-recomp's, which counted at a higher level, so its ranges don't carry over: use `SSBU_SEQ_LOG` to find yours. With `SSBU_GPU_THREAD=0` they are approximate.
- **The log** has one line per press and per wait that ends: `[seq] poll 812: tick 812, step 3 (60:A): keys for 8 ticks`.

Not yet checked in a run:

- that player 1 is polled once per frame: count the polls against the frames shown;
- that the `movie` and `module:<name>` marks appear;
- that `SSBU_SEQ_LOG`'s draw counts are steady on a still screen;
- that a pad in slot 0 and the keyboard are ignored.

## `SSBU_SKIP_MOVIES`

After n samples (12 when n is 1) a movie's stream reports that it has no more data. The game should then end the movie through its normal end-of-stream path, as it did in ssbu-recomp. ssbu-recomp only tried this on the opening movie. It also found that an end before any sample is not one the game gets through, which is why the default is 12 and not 0. The count starts again when the game seeks the stream: it does that when a movie started late, and samples it read before the seek are no use to it.

It works on the movie library's `movie::Extractor` vtable in `subsdk0` ([`hle_movie_skip.c`](../runtime/hle_movie_skip.c)). If that vtable is not as expected, the log says so and movies play as usual.

**A known gap, which matters on a busy PC.** On a late start, the game can seek the stream while its player is still being prepared. By then the stream may already have ended after its 12 samples. The game's decoders have taken that end and offer no more input, so the movie waits forever and the title screen never comes. ssbu-recomp saw this in about 1 boot in 4 when two games booted at once. It fixed it by having its own decoders take inputs again after such a seek. Here the decoders are the game's own, so that fix is not possible. The log says when such a seek happens. On a machine that runs several games or jobs at once (CI), use a larger count: `SSBU_SKIP_MOVIES=120` should let the movie start playing before its stream ends. That is an inference from ssbu-recomp's analysis, not a measurement.

Not yet checked in a run:

- that the log says the `movie::Extractor` vtable was taken, not that movies are left alone;
- that the opening movie ends and the title screen comes, at 1 and at 120;
- how often the seek after an early end hangs here, on repeated boots of a busy PC.

## An example run

A made-up sequence for a run (its draw counts are made up too, so find yours with `SSBU_SEQ_LOG`):

<!-- example sequence: tests/test_test_runs.py parses it -->
```text
# wait for the opening movie, then for the next screen to hold still
@movie
@draws:40-60/30
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
