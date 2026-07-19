# Factory presets

Drop your factory `.gsting` files in THIS folder.

- To create one: shape a sound in the plugin, press **Save** (top-left), then copy the
  file from `Documents\Galactic Sting\Presets\` into this folder.
- When packaging the plugin for distribution, ship this folder **renamed to `Presets`,
  next to the binary** (next to `Galactic Sting.exe`, or inside
  `Galactic Sting.vst3\Contents\x86_64-win\`). On first run the synth copies every
  `.gsting` found there into the user's `Documents\Galactic Sting\Presets\`, so they
  appear in the preset menu automatically.
- User presets always live in `Documents\Galactic Sting\Presets\` — that folder is
  also where the preset combo reads from.
