// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.test;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import org.circuitmc.spi.CircuitPlugin;
import org.circuitmc.spi.PluginContext;

/**
 * Waits for a world query in onEnable, with a bound. /jstartup reports the
 * outcome; with CIRCUIT_TEST_STARTUP_REPORT set, the outcome is also written
 * to that file, for servers stopped during startup.
 */
public final class StartupQueryPlugin implements CircuitPlugin {
    private PluginContext context;
    private volatile String outcome = "not run";

    @Override
    public void onLoad(PluginContext value) {
        context = value;
    }

    @Override
    public void onEnable() {
        final long began = System.nanoTime();
        try {
            final long tick = context.world().time().get(10, TimeUnit.SECONDS);
            outcome = "answered at tick " + tick;
        } catch (TimeoutException e) {
            outcome = "timed out";
        } catch (ExecutionException e) {
            outcome = "failed: " + e.getCause().getMessage();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            outcome = "interrupted";
        }
        outcome += " after " + TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - began) + " ms";
        final String report = System.getenv("CIRCUIT_TEST_STARTUP_REPORT");
        if (report != null) {
            try {
                Files.writeString(Path.of(report), outcome + "\n", StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            } catch (IOException e) {
                System.err.println("cannot write the startup report: " + e);
            }
        }
        context.registerCommand("jstartup", "", (player, args) -> "startup query " + outcome);
    }

    @Override
    public void onDisable() {}
}
