# lib

| Library | Files | |
|---------|-------|-|
| `args` | `args.h`, `args.c` | splits a command line into `argc` / `argv` (quotes keep spaces) and joins it back; parses dotted paths such as `1.1.2.250`; used by the kernel |
| `byte8` | `byte8.h`, `byte8.c` | packet framing over a byte stream with CRC16 (up to 250 data bytes per packet) |
| `ddata` | `ddata.h`, `ddata.c` | sends / receives a buffer in 128-byte fragments through a file descriptor |
| `pidlib` | `pidlib.h`, `pidlib.c` | PID controller (float) with output limits |
| `queue` | `queue.h` | type-generic ring queue, declared by macro |
| `ema_filter` | `ema_filter.h` | exponential moving average in integer arithmetic |
| `medianfilter` | `medianfilter.h` | type-generic running median, declared by macro |
| `uuid` | `uuid.h`, `uuid.c` | PIC24 / dsPIC33 unique device ID (XC16 only) |

Except `uuid`, they build on a PC as well. Host tests: `make -C lib/test`.
