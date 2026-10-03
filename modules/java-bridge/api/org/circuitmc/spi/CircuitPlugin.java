// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.spi;

/** Draft native Circuit SPI; this is not a Bukkit compatibility interface. */
public interface CircuitPlugin {
    void onLoad(PluginContext context) throws Exception;
    void onEnable() throws Exception;
    void onDisable() throws Exception;
}
