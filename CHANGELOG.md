# Changelog

## 0.3.7

- Opus 2 version (`cross.opmod`, beta): the same DSP and screen code as the Hundekraft version, built with the Opus 2 plugin kit. Checked on a computer only, not yet on a real OP-1.
- With MIDI on Opus, velocity sets only the level, not how far the filter opens.
- The Hundekraft version is unchanged (`cross.knl` 0.3.6).

## 0.3.6

First public release.

- Mono bass engine: VCO1 morphing from saw to narrow pulse with a square sub, a detuned VCO2, a four-pole ladder filter with a filter envelope, and level-compensated drive.
- Strictly monophonic in every OP-1 play mode, last-note priority, legato hand-back without restarting the filter envelope.
- Seven controls: CUTOFF, RESONANCE, ENV, DRIVE on page 1; WAVE, DETUNE, DECAY on page 2.
- Live-drawn screen: an upside-down cross in perspective, with a halo, a drive wave and page dots.
- Tested on a real OP-1 under Hundekraft 3.1.2: sound, knobs and screen all work well.
