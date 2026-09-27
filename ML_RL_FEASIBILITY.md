# RL/ML for Haven — what's real, what's buildable now, what isn't

Written 2026-09-25, in response to being asked to look into RL/ML features
that could help figure out what's causing discomfort. Grounded in real
precedent (cited), not speculation. Current state, confirmed exhaustively
this session: zero ML/RL code exists anywhere in any Haven repo today. This
is what could exist, in three honest tiers.

## First, a distinction that decides everything below

- **Tinnitus** (a phantom ringing/buzzing/hissing) is generated internally
  by the auditory nerve. The large majority of cases ("subjective tinnitus")
  have **no external acoustic signature** — there is nothing in a microphone
  recording that corresponds to it. No ML model, no amount of data, can
  detect tinnitus's "cause" from ambient audio, because there usually isn't
  one to detect. The only audio-based approach that works is what Haven
  already has: ask the person to match the *perceived* pitch/loudness
  themselves (the existing 2AFC pitch-match flow). That's not a gap to fill,
  it's already the right approach.
- **Hyperacusis** (pain/discomfort from ordinary external sounds) is
  different — the sound is real and recordable. This is the one where
  "figure out what's causing the pain" is a legitimate ML/DSP problem:
  correlating what's in the environment with when the user reports
  discomfort.

Everything below is about the hyperacusis/external-sound case.

## Tier 1 — buildable now, no dataset, real precedent

**Preference-learning ("dueling bandit") tuner for the filter settings.**
Widex ships almost exactly this in a commercial hearing aid product
(SoundSense Learn): instead of a person manually dragging sliders across a
huge parameter space, the device presents repeated A/B comparisons ("which
sounds better, this or this?") and a bandit algorithm converges on the
person's preferred setting in about a dozen comparisons, out of a space of
2,000+ possible combinations. [Widex's own explanation](https://www.widexpro.com/en-us/widex-technology/artificial-intelligence/sound-sense-learn/).
Academic versions of the same idea exist for hearing aids specifically,
including dueling-bandit and human-in-the-loop RL approaches to personalize
compression and comfort targets ([MDPI review, 2024](https://www.mdpi.com/1424-8220/24/5/1546);
[arXiv:2007.00192](https://arxiv.org/abs/2007.00192), human-in-loop deep RL
for hearing-aid compression).

Haven's existing `MULTI_FILTER` parameter space (`f0`, `Q`, `atten_db` per
band, up to 5 bands) is exactly the kind of space this technique targets.
This needs **no pre-existing dataset** — it learns online, per person, from
their own A/B choices, starting the moment someone uses it. This is the
single most realistic, differentiated thing to build next, and it directly
answers "help me find the setting that actually feels right" without ever
touching the "detect the cause" question dishonestly.

**A bandit-paced tolerance plan.** The existing tolerance-building feature
(`constants/tolerance.ts`) steps `attenDb` down by a fixed 3 dB per week,
always on explicit user tap. A small contextual bandit could instead adapt
the step size or pacing based on the comfort check-in responses already
being collected — still needs no big dataset, since it's learning from one
person's own ongoing feedback, not a pretrained model.

## Tier 2 — buildable, needs more dev work, modest data

**Continuous (not offline-batch) version of the existing PSD tool.** The
`ml/analyze.py` heuristic that already exists (Welch PSD, dominant-peak
half-power bandwidth) currently runs on a single recorded .wav file. Running
the same math over a rolling window of live mic audio — on the phone, or
on-device — turns "what was the troublesome frequency in this recording"
into "what's the troublesome frequency right now." This is still fixed DSP,
not a learned model, and it's the honest, buildable version of "figure out
what in the environment is bothering me."

**On-device sound-event classification.** Nordic officially supports Edge
Impulse's tinyML workflow on the nRF5340 — train a small sound classifier
(traffic, machinery, alarm, speech, quiet) and run it directly on the
Cortex-M33, no cloud, no phone required for inference
([Nordic/Edge Impulse announcement](https://www.edgeimpulse.com/blog/nordic-embedded-machine-learning/)).
This needs real recordings to train on — hours, not the huge corpora deep
learning usually implies, and Edge Impulse's workflow is built for exactly
this scale — but it's a real dataset requirement, unlike Tier 1. Correlating
"this kind of sound was present" with "the user reported discomfort around
this time" is what would let Haven eventually say something like "traffic
noise tends to bother you more than machinery," which is a genuinely useful,
honest claim distinct from "detecting the cause of your tinnitus."

## Tier 3 — don't build, don't claim

- **Anything framed as detecting or diagnosing the source of tinnitus from
  ambient audio.** Not achievable for the majority of cases — see above.
- **Full deep RL from scratch**, as opposed to a bandit. The academic work
  in this space (e.g. the arXiv paper above) still relies on simulated
  patient models to pretrain before ever touching a real user, because one
  person's real interactions are far too few data points to train a deep RL
  policy from nothing. A bandit is the right tool at this data scale; deep
  RL is not, at least not for a v1.

## Recommendation

Start with the Tier 1 dueling-bandit tuner over the existing `MULTI_FILTER`
parameters. It's real, it's precedented in a shipping commercial product,
it needs zero training data, and it replaces "the user guesses at sliders"
with something that's honestly describable as personalization. Tier 2's
on-device classifier is a real second step once there's a reason to collect
the training recordings. Tier 3 stays off the roadmap and out of any
marketing copy.

## Update 2026-09-27: built, and two more real options found

**The Tier 1 bandit tuner is built** — `haven-app` PR #10, "Add a
preference-guided tuner for softening depth and width." Two phases (depth,
then width), same algorithm class described above, 15 new tests including
one that caught a real off-by-one convergence bug before it shipped (an
earlier version of the search could stop one comparison short and silently
lock in the wrong candidate at the edge of the range).

**A "frontier AI model" feature, prototyped**: `haven-app` PR #11 — a
deterministic trend-insights layer over the existing LDL/match history
(real stats, no AI at all, useful on its own), plus a tested-but-not-live
LLM rewrite step that turns those facts into a plain-language paragraph.
The actual safety property that makes this honestly buildable in a
health-adjacent app: every number the model outputs is checked against the
real source data, and any fabricated number (an invented percentage, an
untested frequency) discards the response and falls back to the
deterministic text. **Not wired to a live API key anywhere** — a client app
must never embed a provider key in its bundle, so this needs a small
backend first (holds the key, the app calls that instead), which is a real,
separate cost/ops decision, not something to add unilaterally. Full
writeup: `haven-app/docs/llm-summary.md`.

**Two more real Tier 2 options found, both with concrete precedent:**

- **YAMNet for on-device/phone sound-event classification** — a real,
  publicly available pretrained model (521 sound classes, trained on
  AudioSet) with an existing TFLite conversion and React Native bindings
  ([TensorFlow's own writeup](https://blog.tensorflow.org/2021/09/easy-machine-learning-for-on-device-audio.html);
  [YAMNet→TFLite conversion](https://medium.com/@antonyharfield/converting-the-yamnet-audio-detection-model-for-tensorflow-lite-inference-43d049bd357c)).
  The real advantage over the Tier 2 idea in this doc's first version:
  **no custom training data needed at all** — it's pretrained on a broad
  general-sound corpus, so tagging ambient context (traffic, alarm, speech,
  machinery, quiet) could start immediately instead of waiting on a
  recordings-collection step. The real cost: React Native's TFLite tooling
  needs a custom dev client, not Expo Go or the web build — same
  constraint already blocking `react-native-ble-plx` bring-up, so this
  would ride along with that work rather than needing its own new
  infrastructure. Not prototyped yet — worth doing once a dev-client build
  exists for other reasons anyway.
- **RNNoise for mic noise suppression** — a real, widely-shipped open-source
  library (used in Mumble, OBS) that pairs classic DSP with a small
  recurrent network to suppress background/wind noise in real time
  ([project background](https://hobo.house/2024/03/01/easy-noise-suppression-with-rnnoise/)).
  The genuinely relevant finding: **it's been ported and run in real time on
  an STM32 microcontroller**
  ([Real-Time RNN Speech Noise Suppression on a MCU](https://medium.com/analytics-vidhya/real-time-rnn-speech-noise-suppression-on-a-microcontroller-stm32-e17d8c3eac57)),
  a similar embedded class to the nRF5340's Cortex-M33, with follow-up work
  on mixed FP16/INT8 quantization specifically for multi-core MCU speech
  enhancement ([arXiv:2210.07692](https://arxiv.org/pdf/2210.07692)). This
  is a plausible on-device candidate for cleaning up the PDM mic signal
  (wind/handling noise) before it reaches the hear-through path — distinct
  from anything in Tier 1/2 above, and worth a real feasibility pass
  (memory/cycle budget on the nRF5340 specifically) before committing to it,
  not assumed to fit just because an STM32 port exists.

Neither of these is built. Both are real, cited, and buildable without
inventing a dataset from scratch — a materially better starting position
than this doc's original Tier 2 sketch (which assumed custom-recorded
training data would be needed either way).
