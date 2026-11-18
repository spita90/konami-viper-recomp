# Rumble tests

Run `tests/run-rumble-tests.sh`. No game data is needed.
Uses an SDL virtual device and a recording rumble transport to test torque, stop, expiry duration, throttling, transient failures and retry throttling and tick wrap. SDL 2.24+ is needed for the virtual-device test.

## Physical SDL comparison

Build `tests/manual/build-sdl-rumble.sh`, then quit games and run
`/tmp/viper-sdl-rumble`. It sends three equal one-second 50% pulses through
`SDL_GameControllerRumble`, pumping SDL events throughout. It does not call our
Apple backend. Exactly one recognized controller must be connected.

Use `--list` for discovery without vibration, or `--driver hidapi` / `--driver native`
to compare SDL routing preferences. The latter disables HIDAPI; the former enables
it but does not guarantee SDL selects that driver. Logs include runtime version,
revision, requested driver hints, device path, capability and submission errors.
An accepted API call is not proof of physical vibration. Record whether the pulses
are equal, fade, are silent, or disconnect the controller. Ctrl-C stops the test.
