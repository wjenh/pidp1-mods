# Apollo 11 WAV example

Source page: https://science.nasa.gov/resource/sounds-of-mars-one-small-step/

Downloaded WAV: https://science.nasa.gov/wp-content/uploads/2024/04/sounds-of-mars-one-small-step-earth.wav

Credit: NASA/JPL-Caltech. Apollo 11 astronaut Neil Armstrong's transmission confirming the first footsteps on the Moon. This is the **Earth** version offered on NASA's page, not its simulated Mars-atmosphere version.

Original source: 48000 Hz, two-channel signed 16-bit PCM; 8.975625 seconds. SHA-256: `9006c290b776d9163158ff3013710ce78af5634a1091925004b4d6f9de044990`.

The imported example uses equal-weight channel mixing, anti-alias resampling to 4096 Hz, explicit amplitude gain 2, then nearest-integer 18-bit one's-complement quantization. No automatic normalization, DC removal, windowing or host FFT was applied. The source WAV is retained unchanged. Credit/source and all conversion settings are recorded in `Drums/Apollo11.drm.json` on the Pi.
