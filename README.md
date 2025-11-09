# Gamma LUT Utility

`gamma` is a small command-line helper for Linux DRM systems that loads a custom
color look-up table (LUT) into a display controller (CRTC). It can apply
ad-hoc tweaks, recall named presets, or reset the hardware gamma ramp back to
linear values.

## Features

- Works directly with the Direct Rendering Manager (DRM) atomic API.
- Accepts on-the-fly adjustments or INI-based presets.
- Provides built-in safety clamping to avoid extreme values.
- Supports listing and resetting presets without touching the LUT.

## Prerequisites

- A Linux system with `/dev/dri/card*` devices exposed.
- Development headers for `libdrm`.
- A C toolchain (tested with `gcc`).

On Debian/Ubuntu systems you can install the dependencies with:

```sh
sudo apt install build-essential libdrm-dev
```

## Building

Use `make` to compile the utility. The provided `Makefile` uses a reasonable
set of flags and links the required DRM libraries.

```sh
make
```

This creates the `gamma` binary in the project root.

## Usage

The program supports three primary invocation patterns:

```sh
./gamma [--crtc <id>] <gamma_pow> [lift gain r g b]
./gamma [--crtc <id>] <preset-name>
./gamma --list
```

- `--crtc <id>` overrides the display controller (CRTC) to configure. When
  omitted, the compiled-in default (`DEFAULT_CRTC`, currently `68`) is used.
- `<gamma_pow>` is the exponent used to shape the curve. Additional values
  allow you to refine the lift, gain, and per-channel multipliers.
- `<preset-name>` loads parameters from an INI file (see below).
- `--list` prints every preset discovered in `presets.ini` and
  `/etc/gamma-presets.ini` without modifying the hardware state.

The special preset name `reset` restores a linear LUT (gamma 1.0, lift 0.0,
unit gain, and neutral RGB multipliers).

## Presets

Presets live in simple INI files and are loaded in the following order:

1. `./presets.ini`
2. `/etc/gamma-presets.ini`

Section names map to preset names that can be supplied on the command line.
Each section accepts the keys `gamma`, `lift`, `gain`, `r`, `g`, `b`, and an
optional `crtc`. Keys can be omitted, in which case defaults are used.

Example (`presets.ini`):

```ini
[milos1]
gamma=0.85
lift=-0.10
gain=1.40
r=1.25
g=1.22
b=1.12
```

Run `./gamma milos1` to apply this preset.

## Tuning the Variables

The numeric mode lets you experiment quickly. Every parameter is validated to
stay within safe ranges defined in `gamma.c`.

| Parameter | Range | Effect |
|-----------|-------|--------|
| `gamma_pow` | 0.20 – 5.00 | Raises pixel values to this exponent. Values < 1 brighten midtones; > 1 darken them. |
| `lift` | −0.50 – 0.50 | Adds an offset before the gamma curve. Negative numbers deepen shadows; positive numbers lift blacks. |
| `gain` | 0.50 – 1.50 | Multiplies the result after the gamma curve, broadly affecting contrast. |
| `r`, `g`, `b` | 0.50 – 1.50 | Per-channel multipliers applied after gain. Use them to adjust white balance or color bias. |

A typical workflow for manual tuning is:

1. Start with a neutral baseline: `./gamma 1.0 0.0 1.0 1.0 1.0 1.0`.
2. Adjust `gamma_pow` to control overall brightness of midtones.
3. Nudge `lift` for shadow detail; keep it subtle to avoid clipping blacks.
4. Increase or decrease `gain` to fine-tune highlights.
5. Modify `r`, `g`, `b` in small increments (±0.05) to correct color casts.
6. When satisfied, record the values in `presets.ini` under a new section for
   future reuse.

Remember to use `--list` to confirm that your preset is discoverable, and the
`reset` preset to return to the factory LUT when needed.

## Using ChatGPT to Generate Presets from Reference Images

If you prefer an AI-assisted workflow, you can ask ChatGPT to compare a target
image against a reference and suggest matching parameters. Provide both images
and include the following context so the model knows exactly what to deliver:

1. **Describe the utility and its parameters.** Remind ChatGPT that
   `./gamma <gamma_pow> <lift> <gain> <r> <g> <b>` applies a LUT to the selected
   CRTC and that presets in `presets.ini` use the keys `gamma`, `lift`, `gain`,
   `r`, `g`, `b`, and optional `crtc`.
2. **Attach or reference the two images.** Clearly label them as "Image1" and
   "Image2" (or similar) and state which is the baseline and which represents
   the desired look.
3. **Ask for both outputs.** Request (a) a preset block that you can paste into
   `presets.ini`, and (b) a ready-to-run command line in numeric mode.
4. **Specify acceptable ranges.** Mention the valid spans listed in the table
   above so the suggestions stay within hardware-safe limits.

### Prompt Template

```text
You are helping me tune the radxa-zero3 gamma LUT tool.
- The command syntax is: ./gamma [--crtc <id>] <gamma_pow> <lift> <gain> <r> <g> <b>
- Presets in presets.ini use: gamma=, lift=, gain=, r=, g=, b=
- Valid ranges: gamma_pow 0.20-5.00, lift -0.50-0.50, gain 0.50-1.50, r/g/b 0.50-1.50
Compare Image1 (current output) and Image2 (target look) and suggest values.
Return:
1. A preset named <my_preset_name> formatted for presets.ini.
2. A ./gamma command that applies the same settings.
```

ChatGPT should respond with both the preset block and the direct command, ready
for you to apply or tweak further.

## Troubleshooting

- **`open /dev/dri/cardN: No such file or directory`** – Ensure you are running
  on a DRM-enabled system and have sufficient permissions (often root or video
  group membership).
- **`Preset '<name>' not found.`** – Verify the section name in your INI file
  and that the file resides in one of the search paths above.
- **`<parameter> out of range`** – The tool clamps inputs to keep displays
  usable. Revisit your values and ensure they fall within the allowed ranges.

## License

This project does not currently declare a license. Consult the repository
owner if you need clarification on usage rights.
