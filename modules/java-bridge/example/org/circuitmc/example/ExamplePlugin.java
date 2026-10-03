// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.example;

import java.util.Objects;
import org.circuitmc.spi.CircuitPlugin;
import org.circuitmc.spi.PluginContext;

/** Compile-only lifecycle example; no server host or discovery exists yet. */
public final class ExamplePlugin implements CircuitPlugin {
    private PluginContext context;

    @Override
    public void onLoad(PluginContext value) {
        context = Objects.requireNonNull(value);
    }

    @Override
    public void onEnable() {
        context.logger().log(System.Logger.Level.INFO,
                "Enabled Circuit SPI example: " + context.pluginId());
    }

    @Override
    public void onDisable() {
        context.logger().log(System.Logger.Level.INFO, "Disabled Circuit SPI example");
    }
}
