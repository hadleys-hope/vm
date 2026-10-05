# HopeVM

This stage reads HBC v1, initializes globals, executes HopeLang functions, supports host builtins/events, and runs START/EVERY/AT handlers on virtual time.

Build (the VM library, the `hopevm` CLI, `hope-runtime`, `bus-bench` and the unit tests):

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
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

## House I/O

A program that controls a house gets four host functions (declared in the compiler's `Builtins.kt`):

| Function | Meaning |
|---|---|
| `sense(name of string) of real` | a reading from the house's last sensors message or the shared `hh/env/*` topics; booleans are 0.0 / 1.0, an unknown name is 0.0 |
| `act(name of string, value of real)` | set an actuator; `heater_on`, `valve_open`, `appliances_on` are sent as booleans |
| `act_text(name of string, value of string)` | a text actuator, e.g. `program`, `reason` |
| `house_id() of int` | the house this VM controls |

`log(...)` from a house program is published to `hh/house/{id}/log` instead of stdout.

The host posts the event `Sensors()` when a reading arrives, so a house program declares `event Sensors()` and
handles `on Sensors()`. Timed handlers (`every`, `at`) run on the world's clock: one tick is one simulated minute.

## Step budget

`VM::setStepBudget(n)` limits how many loop iterations (backward jumps) and calls a handler may make; past it the
handler stops with `BudgetExceeded`. Straight-line code is not metered: it cannot run away, and metering only
jumps and calls keeps the cost below the noise of the interpreter loop.

## hope-runtime

One process hosts one VM per house and connects them to the world over MQTT (the same topics as the Python
controllers in `world`):

```bash
hope-runtime --programs comfort.hbc,eco.hbc,night_setback.hbc,storm_ready.hbc,dumb.hbc \
             --houses 5000 --threads 4 --mqtt mosquitto:1883
```

- in: `hh/house/{id}/sensors`, `hh/env/weather`, `hh/env/power`; out: `hh/house/{id}/actuators`,
  `hh/house/{id}/log`, `hh/runtime/status` (every 2 s);
- house `i` runs `programs[i % k]`; houses are sharded over worker threads by `id % threads`, a VM is only touched
  by its own worker, so the VMs need no locks;
- a shard keeps only the newest reading per house: a worker that falls behind drops stale readings instead of
  queueing them, so memory and latency stay bounded under overload;
- a handler that throws or runs out of budget is a fault: the VM is rebuilt on the next reading; after 3 faults the
  house is quarantined for 600 ticks and the world's built-in thermostat takes over;
- the MQTT client (`src/mqtt`) is a minimal MQTT 3.1.1 client, QoS 0, written for this project; on a lost
  connection the runtime reconnects with backoff and resubscribes.

Batched bus (for thousands of houses; the world runs with `--mqtt-batch`): the world sends one
`hh/batch/sensors` message per tick with the readings as columns (`{"t":..,"id":[..],"t_in":[..],..}`), and a house
that came in a batch is answered in `hh/batch/actuators` (`{"rows":[{"id":..,"heater_on":..,..},..]}`, one message per
worker pass). Both modes work at the same time.

`--bench R` runs without a broker: every house gets R synthetic readings, and the throughput is printed.

## bus-bench

Plays the world on the bus to load-test the broker and the runtime:

```bash
bus-bench --mqtt localhost:1883 --houses 5000 --tick-hz 20 --period 10 --seconds 10 --monitor
```

Each tick it sends the sensors of the houses whose phase is due (`id % period == tick % period`, as the world
does) and measures the replies and their latency; `--monitor` adds a subscriber to `hh/#`.

## Measured (one shared core of a cloud sandbox, `-O2`)

| | |
|---|---|
| 5000 VMs built | 27 ms, 6.7 KB RSS per house (53 MB in total) |
| `--bench`, 5000 houses, 1 thread | 117 000 readings/s; handler p50 4.1 us, p99 34 us |
| broker + runtime, 5000 houses, 20 ticks/s, every house every 10 ticks, with a `hh/#` monitor | 10 060 readings/s in, 10 060 replies/s out, 20 120/s to the monitor; all replied, p99 48 ms; Mosquitto 16 % and the runtime 12 % of the core |
| the same at every house every tick | 39 600/s each way, all replied, p99 460 ms: the single core is saturated by the generator, broker and runtime together |
| world (Python) + Mosquitto + runtime, 5040 houses, batched bus | 20 ticks/s held: world tick 15.9 ms, publish 3.5 ms (40 ms per house-message), every house under VM control |
| 500 of 1000 houses running an endless loop | each faulted 3 times and was quarantined; the other 500 answered every reading |
