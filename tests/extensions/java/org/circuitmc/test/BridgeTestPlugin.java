// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.test;

import org.circuitmc.spi.CircuitPlugin;
import org.circuitmc.spi.PluginContext;

/**
 * Failure scenarios for the bridge tests: /jcrash ends the JVM at once,
 * /jstall blocks the plugin thread, /jflood sends many requests.
 */
public final class BridgeTestPlugin implements CircuitPlugin {
    private PluginContext context;
    private volatile boolean stalled;

    @Override
    public void onLoad(PluginContext value) {
        context = value;
    }

    @Override
    public void onEnable() {
        context.registerCommand("jcrash", "", (player, args) -> {
            Runtime.getRuntime().halt(3);
            return "unreachable";
        });
        context.registerCommand("jstall", "", (player, args) -> {
            stalled = true;
            return "stalling";
        });
        context.registerCommand("jflood", "", (player, args) -> {
            for (int i = 0; i < 20000; ++i) context.world().time();
            return "flooded";
        });
        context.onTick(tick -> {
            while (stalled) Thread.sleep(1000);
        });
    }

    @Override
    public void onDisable() {}
}
