# hub75_zephyr_driver

A Zephyr out-of-tree driver for HUB75-compatible RGB LED matrix panels. It implements Zephyr's standard `display` driver API, allowing any display-aware application or library to target HUB75 panels without modification.

## How it works

HUB75 panels are driven by serially shifting pixel data into internal shift registers, then latching and selecting the active row via address lines (A–E). Because the panel has no internal framebuffer or self-refresh logic, the driver must continuously scan all rows to sustain a visible image.

The driver uses two techniques to achieve this:

**Binary Code Modulation (BCM):** Rather than simple on/off PWM, the driver uses BCM to produce up to 8-bit per channel color depth. Each refresh pass iterates over bit-planes (bit 0 through bit 7), holding each plane active for a duration proportional to its bit weight (1 µs, 2 µs, 4 µs … 128 µs). This yields 256 intensity levels per channel with far less CPU overhead than traditional PWM scanning.

**Hardware counter-driven ISR:** A hardware counter (configured via devicetree) fires a channel alarm callback that shifts out one row's current bit-plane, latches it, selects the row address, and re-arms the alarm for the next bit-plane duration. All panel scanning happens from this ISR, keeping the application thread free.

The driver exposes a framebuffer in 24-bit RGB format (8 bits per channel). Calling `display_write()` copies the provided buffer into the internal framebuffer, which is picked up on the next scan pass.

## Pixel format

24-bit RGB, 1 byte each for R, G, B per pixel, row-major order. The top half of the panel corresponds to the R1/G1/B1 data lines and the bottom half to R2/G2/B2.

## Devicetree binding

Compatible string: `zephyr,hub75`

Required properties:

| Property | Description |
|---|---|
| `width` | Panel width in pixels |
| `height` | Panel height in pixels |
| `counter` | Phandle to a hardware counter instance |
| `pin-r1-gpios` | Red data, top half |
| `pin-r2-gpios` | Red data, bottom half |
| `pin-g1-gpios` | Green data, top half |
| `pin-g2-gpios` | Green data, bottom half |
| `pin-b1-gpios` | Blue data, top half |
| `pin-b2-gpios` | Blue data, bottom half |
| `pin-a-gpios` | Row address bit 0 |
| `pin-b-gpios` | Row address bit 1 |
| `pin-c-gpios` | Row address bit 2 |
| `pin-d-gpios` | Row address bit 3 |
| `pin-e-gpios` | Row address bit 4 |
| `pin-clk-gpios` | Shift register clock |
| `pin-latch-gpios` | Shift register latch |
| `pin-n-en-gpios` | Output enable (active low) |

## Kconfig

Enable with `CONFIG_HUB75=y`. The driver automatically selects `DISPLAY`, `GPIO`, and `COUNTER`.

## Limitations / known issues

- `display_write()` copies the entire framebuffer; partial writes (x/y offsets) are not yet honored.
- `get_capabilities`, `set_brightness`, `set_contrast`, `set_pixel_format`, and `set_orientation` return `-ENOTSUP`.

## TODO

- [ ] **VSync support**: synchronize framebuffer swaps with the scan cycle to eliminate tearing.
- [ ] **QSPI transfer investigation**: explore using QSPI to shift pixel data out faster and reduce ISR CPU time.
- [ ] **Optional dedicated work queue**: fall back to a work-queue-based scan loop for targets without a suitable hardware timer.
- [ ] **DMA options**: investigate DMA-driven data transfer to offload the bit-banged shift loop from the CPU entirely.
