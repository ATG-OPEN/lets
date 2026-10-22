# lib

| Library | Files | |
|---------|-------|-|
| `args` | `args.h`, `args.c` | splits a command line into `argc` / `argv` (quotes keep spaces) and joins it back; parses dotted paths such as `1.1.2.250`; used by the kernel |
| `iir` | `iir.h`, `iir.c` | 2nd-order IIR section (biquad): RBJ design from two cut-offs (low only = high-pass, high only = low-pass, both = band-pass). Float runs that start in the steady state for a given offset; Q14 fixed point (`iir_q14_*`, `IIR_Q14_OUT`) header-only, for MCUs without an FPU. |
| `pulse` | `pulse.h`, `pulse.c` | returns in a sampled trace (rangefinder, lidar, sonar): robust noise (lower quartile of detrended block RMS), peaks with a 50 % constant-fraction edge, SNR, up to 4 returns strongest first, blanking of the near zone |
| `apdbias` | `apdbias.h`, `apdbias.c` | automatic avalanche-photodiode bias: noise floor, step search to the avalanche edge, hold at or below it and follow it; waits for a slow HV supply to settle |
| `apdcurrent` | `apdcurrent.h`, `apdcurrent.c` | avalanche-photodiode bias set by the APD current: amplifier offset at 0 V, then the bias where (level - offset) x scale is the target current, found in seconds behind a slow HV supply (outer loop on the measured bias) and followed |
| `byte8` | `byte8.h`, `byte8.c` | packet framing over a byte stream with CRC16 (up to 250 data bytes per packet) |
| `ddata` | `ddata.h`, `ddata.c` | sends / receives a buffer in 128-byte fragments through a file descriptor |
| `pidlib` | `pidlib.h`, `pidlib.c` | PID controller (float) with output limits |
| `queue` | `queue.h` | type-generic ring queue, declared by macro |
| `ema_filter` | `ema_filter.h` | exponential moving average in integer arithmetic |
| `medianfilter` | `medianfilter.h` | type-generic running median, declared by macro |
| `uuid` | `uuid.h`, `uuid.c` | PIC24 / dsPIC33 unique device ID (XC16 only) |
| `spiral` | `spiral.h`, `spiral.c` | Archimedean spiral walked at constant speed: point and tangent by path length, rewind by time, duration - a 2-axis search pattern |

Except `uuid`, they build on a PC as well (`make test` compiles them all). `iir`, `pulse`, `apdbias`, `apdcurrent` and `spiral` are plain C99 with no kernel, no allocation and no
registers: use them in any project. Host tests: `make -C lib/test`.

```c
iir_biquad_t f;
iir_design(&f, 0, 5e6f, 50e6f);         // 5 MHz low-pass at 50 MS/s
iir_run_i32(&f, trace, n, offset);      // in place, no start-up step

iir_q14_t q;                            // the same section in fixed point
iir_to_q14(&f, &q);
iir_q14_state_t s;
iir_q14_reset(&q, &s, x0);
int32_t y = iir_q14_step(&q, &s, x);
```
