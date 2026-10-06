// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.spi;

/**
 * A Circuit Java plugin. The bridge calls these methods on the plugin thread,
 * one at a time. This is Circuit's own API, not a Bukkit compatibility layer.
 */
public interface CircuitPlugin {
    /** Called once with the plugin's context before {@link #onEnable()}. */
    void onLoad(PluginContext context) throws Exception;

    /** Register commands and listeners here. */
    void onEnable() throws Exception;

    /** Called before the bridge stops; release resources here. */
    void onDisable() throws Exception;
}
