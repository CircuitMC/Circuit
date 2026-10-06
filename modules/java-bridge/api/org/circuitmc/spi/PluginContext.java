// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.spi;

/**
 * What a plugin may use. The world is never touched directly: queries read the
 * state published after the last tick, and changes are submitted as commands
 * that the server applies at the start of a later tick. What a plugin may do is
 * limited by the permissions the server operator granted to the Java bridge.
 */
public interface PluginContext {
    int SPI_VERSION = 2;

    String pluginId();

    System.Logger logger();

    /** Queries and commands; results complete asynchronously. */
    World world();

    /**
     * Registers a chat command, without the leading slash. The handler runs on
     * the plugin thread and its return value is sent to the player.
     */
    void registerCommand(String name, String usage, CommandHandler handler);

    /** Receives every committed tick with its block changes. */
    void onTick(TickListener listener);
}
