PingPongDelay integration notes
- API: setDiv(int) replaces GridDiv usage; already wired in DspPipeline.prepare to initialize 0.
- BPM: transport BPM is propagated each processing block to both dubDelay and pingPongDelay.
- Post-duck path: sampler buffers (tempBuffer in mono, tempBufL/tempBufR in stereo) are ducked and then fed into delays, with results mixed back into master path after delays.
- Process chain: In DspPipeline, after sampler processing, delays are invoked via processAdd with post-duck buffers to produce the final left/right outputs.
- Tests: existing unit tests plus integration tests cover core paths; latency tests were adjusted for slower environments.
