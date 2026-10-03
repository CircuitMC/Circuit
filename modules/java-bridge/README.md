# Circuit Java bridge scaffold

This directory contains the Circuit Java SPI draft and a lifecycle example for Java 17 or later. The framework does not yet start a JVM, discover plugins, transport messages, or execute these plugins. It does not implement Bukkit, Spigot, or Paper APIs.

## Planned runtime

The first bridge uses an independent JVM sidecar with authenticated local IPC. Define:

- JVM and per-plugin lifecycle, ClassLoader handling, and SPI compatibility.
- Versioned bounded messages, request IDs, errors, deadlines, cancellation, and backpressure.
- Immutable event snapshots and controlled command submission to authoritative owners.
- Failure handling for process exit, oversized messages, protocol mismatch, and queue overload.
- Shutdown draining and ownership of strings, buffers, requests, and returned data.

Chunk workers must not wait indefinitely on Java futures or execute arbitrary plugin callbacks. Any synchronous decision API requires explicit compatibility and timeout semantics. Embedded JVM/JNI integration is a possible later alternative, not the initial implementation requirement.

ClassLoader isolation is not a security sandbox. A sidecar also needs suitable OS permissions. Existing Bukkit/Paper plugin compatibility requires a separate specification and test matrix.

## Compile the SPI example

From the main repository root:

```bash
mkdir -p build/java-spi
javac --release 17 -d build/java-spi \
  modules/java-bridge/api/org/circuitmc/spi/*.java \
  modules/java-bridge/example/org/circuitmc/example/*.java
```

`tools/check.sh` performs this check when `javac` is available. Successful compilation verifies source interfaces, not Java plugin execution.
