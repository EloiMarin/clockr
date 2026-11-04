// Stirred Field's audio voice. Not a clockr engine — a plain consumer of
// the scene's folded state: the wasm recipe produces `pitch` / `level` (and
// throws / wall hits / swim darts arrive as one-shot events from the page;
// the deep kick's period arrives with them). Everything slews toward
// targets so the worker cadence's 50 ms refresh reads as one continuous
// sound.
//
// Register set by the reference in /tmp/reference.wav: the first half's
// dusty, low-sheen atmosphere lives on as the flow bed, at a whisper; the
// second half's deep kick is the scene's heartbeat — slow, soft-nosed dub
// with a pitch-fallen sub, drum-in-a-fog, never a dancefloor pulse.

class SceneVoice extends AudioWorkletProcessor
{
    constructor()
    {
        super();

        this.sr = sampleRate;

        this.phi = 0;
        this.phiLfo = 0;
        this.dustHoldPhi = 0;

        // Ambience state.
        this.pitch = 147;
        this.tp = 147;
        this.level = 0;
        this.tl = 0;
        this.motion = 0;
        this.tm = 0;

        // Flow bed.
        this.flowPhi = 0;
        this.flowD = 0;
        this.flowLp = 0;
        this.flowBand = 0;
        this.crush = 0;

        // Deep kick: the heartbeat of the scene, seconds long. Time since
        // the hit counts forward (monotonic): idle = large, hit = 0.
        this.kickPeriod = 12;
        this.tk = 12;
        this.kickCountdown = 1.5; // a moment of silence while it arms
        this.subPhi = 0;
        this.subT = 1e9;

        this.bells = [];
        this.noiseSeed = 2.35;
        this.port.onmessage = (event) => {
            const m = event.data;

            if (m.t === "p")
            {
                this.tp = m.p;
                this.tl = m.l;
                this.tm = m.m;

                if (Number.isFinite(m.q) && m.q > 2)
                    this.tk = m.q;
            }
            else if (m.t === "e" && this.bells.length < 6)
            {
                this.bells.push({ phi: 0, phi2: 0, phi3: 0, env: 1, hz: m.hz, g: m.g, dur: m.d ?? 0.28, amp: m.a ?? 0.42 });
            }
        };
    }

    static get parameterDescriptors()
    {
        return [];
    }

    process(_inputs, outputs)
    {
        const out = outputs[0][0];
        const sr = this.sr;
        const n = out.length;
        const slew = 1 - Math.exp(-n / (0.09 * sr));

        this.pitch += (this.tp - this.pitch) * slew;
        this.level += (this.tl - this.level) * slew;
        this.motion += (this.tm - this.motion) * slew;
        this.kickPeriod += (this.tk - this.kickPeriod) * slew;

        // Cut-off breath: `level` swells, a tiny LFO keeps it from sitting dead.
        const hum = this.level * (0.85 + 0.15 * Math.sin(this.phiLfo));
        for (let i = 0; i < n; i += 1)
        {
            const amb = this.pitch * (1 + 0.006 * Math.sin(this.phiLfo * 5.3));

            this.phiLfo += 2 * Math.PI * 0.21 / sr;
            this.phi += 2 * Math.PI * amb / sr;

            let s = 0.42 * Math.sin(this.phi)
                + 0.22 * Math.sin(this.phi * 1.5018 + 0.6)
                + 0.08 * Math.sin(this.phi * 2.71 + 1.9) * Math.min(1, this.level * 2.2);

            // Dust: white noise crushed to a slow grain, then through the
            // one-pole formant — the reference's "dusty atmosphere", but at
            // a whisper.
            this.noiseSeed = (1103515245 * this.noiseSeed + 12345) % 2147483648;
            const nz = (this.noiseSeed / 2147483648) * 2 - 1;
            this.dustHoldPhi += 1400 / sr;
            if (this.dustHoldPhi >= 1)
            {
                this.dustHoldPhi -= 1;
                this.crush = 0.6 * this.crush + 0.4 * nz;
            }
            const dusty = 0.4 * nz + 0.6 * this.crush;
            this.flowPhi += 2 * Math.PI * (300 + 1300 * this.motion * this.motion) / sr;
            this.flowD = Math.sin(this.flowPhi) * 0.22 * dusty;
            this.flowBand = this.flowD - this.flowLp - 0.52 * this.flowBand;
            this.flowLp += 0.14 * this.flowBand;

            const bed = (s + 0.9 * this.flowBand * (0.13 + 0.87 * this.level)) * hum;

            // The deep kick: a heartbeat of seconds. Soft 30 ms nose, then a
            // near-decade drop from 46 Hz toward 33 Hz as its body decays.
            this.subT += 1 / sr;
            this.kickCountdown -= 1 / sr;

            if (this.kickCountdown <= 0)
            {
                this.kickCountdown += this.kickPeriod;
                this.subPhi = 0;
                this.subT = 0;
            }

            let kick = 0;

            if (this.subT < 2.2)
            {
                const body = 33 + 13 * Math.exp(-this.subT * 1.4);
                const nose = Math.min(1, this.subT / 0.03);
                const plus = this.subT < 0.03 ? nose * nose : Math.exp(-(this.subT - 0.03) * 1.6);

                this.subPhi += 2 * Math.PI * body / sr;
                kick = Math.sin(this.subPhi) * plus * plus * (0.11 + 0.05 * this.level);
            }

            out[i] = Math.tanh(bed + kick) * 0.9;
        }

        // Event bells: simple decaying partial stacks.
        for (let i = this.bells.length - 1; i >= 0; i -= 1)
        {
            const b = this.bells[i];
            const step = n / (b.dur * sr);

            for (let j = 0; j < n; j += 1)
            {
                b.phi += 2 * Math.PI * b.hz / sr;
                b.phi2 += 2 * Math.PI * (b.hz * 2.763) / sr;
                b.phi3 += 2 * Math.PI * (b.hz * 5.401) / sr;
                b.env = Math.max(0, b.env - step);

                const env = b.env * b.env;
                out[j] += b.g * env * (
                    Math.sin(b.phi)
                        + 0.4 * Math.sin(b.phi2)
                        + 0.16 * Math.sin(b.phi3) * env
                );
            }

            if (b.env <= 0)
                this.bells.splice(i, 1);
        }

        // Copy into the second channel and soft-gate the tap tail.
        const right = outputs[0][1];

        if (right)
        {
            for (let i = 0; i < n; i += 1)
                right[i] = out[i] * 0.92 + ((i % 2) ? 0.004 * out[i] : -0.004 * out[i]);
        }

        return true;
    }
}

registerProcessor("lab-scene", SceneVoice);
