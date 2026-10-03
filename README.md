# HopeVM stage 4

This stage reads HBC v1, initializes globals, executes HopeLang functions, supports host builtins/events, and runs START/EVERY/AT handlers on virtual time.

Build:

```bash
cmake -S . -B build
cmake --build build -j
```

Initialize a module:

```bash
./build/hopevm ../../compiler/build/hadleys-hope.hbc
```

Run a zero-argument HopeLang function:

```bash
./build/hopevm ../../compiler/build/hadleys-hope.hbc --run houses_decide
./build/hopevm ../../compiler/build/hadleys-hope.hbc --run houses_demand
```

Run the scheduler until an absolute virtual time in milliseconds:

```bash
./build/hopevm ../../compiler/build/hadleys-hope.hbc --simulate 60000
./build/hopevm ../../compiler/build/hadleys-hope.hbc --simulate 10800000
```

Scheduling semantics:

1. globals are initialized in declaration order;
2. all START handlers run once in registration order;
3. queued events are drained;
4. AT 0 handlers run;
5. EVERY/AT handlers execute on virtual time without sleeping;
6. events emitted by timed handlers are drained after all handlers due at that virtual timestamp.

Built-in host functions include `random_real`, `random_int`, `clamp`, `sqrt`, `sin`, `cos`, `log`, and `metric`.
`EMIT` appends to a FIFO event queue; EVENT handlers run in registration order.
