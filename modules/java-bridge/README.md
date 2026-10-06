# Circuit Java bridge

Java plugins run in a separate JVM that the server starts and talks to over an authenticated local socket. Plugins implement the Circuit SPI in `api/`, which is Circuit's own API: it is not Bukkit, Spigot or Paper (see [the evaluation](../../docs/BUKKIT_EVALUATION.md)). Java 17 or later is required.

| Directory | Content |
| --- | --- |
| `api/` | The SPI: `CircuitPlugin`, `PluginContext`, `World`, `CommandHandler`, `TickListener`, `CircuitException` |
| `runtime/` | The bridge JVM: connection, handshake, plugin thread and discovery |
| `example/` | `ExamplePlugin`: `/pulse` places a redstone block for some ticks, `/block` reports a block, and it counts block changes |

## Running

Compile the three directories into one class directory or jar:

```bash
mkdir -p build/java-bridge
javac --release 17 -d build/java-bridge $(find modules/java-bridge -name '*.java')
```

Then configure the server:

```text
java-bridge=build/java-bridge
java-plugin-class=org.circuitmc.example.ExamplePlugin
java-plugins=plugins/java
java-permissions=world.read,world.write,world.observe,commands
```

- **`java-bridge`:** the class path of the runtime and SPI.
- **`java-plugin-class`:** may be repeated; names plugin classes on that class path.
- **`java-plugins`:** a directory of plugin jars. They are found with `ServiceLoader` (`META-INF/services/org.circuitmc.spi.CircuitPlugin`), each jar with its own class loader.
- **`java`:** the Java executable; the default is `java` on the `PATH`.

## Model

**Plugin thread.** Plugin code runs on one plugin thread in the JVM.

**Queries and commands.**
- **Queries** (`time`, `blockAt`, `itemsInChunk`) read the state the server published after the last tick.
- **Commands** (`setBlock`, `useBlock`) are admitted like player input. They are logged for replay and applied at the start of a later tick. The future completes with the admission number when the command is admitted, not when it takes effect.

Results arrive asynchronously. A callback may wait on a future, but this delays the next tick it receives.

**Chat commands.** They are registered by name. The server forwards a player's command to the JVM and sends the reply to the player.

**Ticks.** A subscribed plugin receives every committed tick with its block changes, in order.

**Permissions.** The operator grants the bridge a set of permissions: `world.read`, `world.write`, `world.observe` and `commands`. A refused request completes with `CircuitException` and reason `DENIED`. By default only `world.read` is granted.

## Isolation and failure

The server never waits for the JVM:
- **Requests:** a reader thread queues them, and the game thread answers them between ticks, up to a fixed number per tick.
- **Ticks and replies:** these leave through a bounded queue.

The bridge is stopped, its commands are removed and the server keeps running when one of these happens:
- the JVM exits or closes the connection;
- it sends a malformed or oversized frame;
- it falls more than 200 ticks behind in acknowledging ticks;
- it fills its queues.

**Authentication.** The socket lives in a private directory (mode 0700). The JVM must present a random token, passed in its environment, as its first message. The server compares it in constant time.

**Not a sandbox.** A JVM and class loaders do not isolate plugins from the operating system. Run only trusted plugins, under an account with suitable permissions.

## Protocol (version 1)

**Framing.** A frame is a big-endian 32-bit length (1 to 1 MiB) followed by a payload. The payload is a type byte and fields in `DataOutputStream` encoding: big-endian integers and doubles, and strings as modified UTF-8 with a 16-bit length.

| Type | Direction | Fields |
| --- | --- | --- |
| 1 `HELLO` | to server | `int` version, `UTF` token |
| 2 `LOG` | to server | `byte` level (0 info, 1 warning, 2 error), `UTF` text |
| 3 `REQUEST` | to server | `long` request id, `byte` operation, operation fields |
| 4 `TICK_DONE` | to server | `long` tick |
| 5 `COMMAND_REPLY` | to server | `long` invocation, `UTF` text |
| 6 `READY` | to server | `int` count, `UTF` plugin ids |
| 101 `WELCOME` | to bridge | `int` version, `UTF` bundle id, `int` permission bits (1 read, 2 write, 4 observe, 16 commands) |
| 102 `RESPONSE` | to bridge | `long` request id, `byte` status (0 ok, 1 denied, 2 invalid, 3 unavailable), result fields or a `UTF` message |
| 103 `TICK` | to bridge | `long` tick, `int` count, changes (`int` x, y, z, `UTF` before, `UTF` after) |
| 104 `COMMAND` | to bridge | `long` invocation, `UTF` player, `UTF` name, `int` count, `UTF` arguments |
| 105 `SHUTDOWN` | to bridge | none |

| Operation | Fields | Result |
| --- | --- | --- |
| 1 time | none | `long` tick |
| 2 block at | `int` x, y, z | `UTF` state notation |
| 3 items in chunk | `int` chunk x, z | `int` count, items (`long` id, `double` x, y, z, `UTF` item, `int` count) |
| 4 set block | `int` x, y, z, `UTF` state | `long` admission |
| 5 use block | `int` x, y, z | `long` admission |
| 6 register command | `UTF` name, `UTF` usage | none |
| 7 subscribe to ticks | none | none |
