// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.bridge;

import java.text.MessageFormat;
import java.util.ResourceBundle;

/** A plugin logger that forwards to the server log. */
final class BridgeLogger implements System.Logger {
    private final Bridge bridge;
    private final String plugin;

    BridgeLogger(Bridge bridge, String plugin) {
        this.bridge = bridge;
        this.plugin = plugin;
    }

    @Override
    public String getName() {
        return plugin;
    }

    @Override
    public boolean isLoggable(Level level) {
        return level.getSeverity() >= Level.INFO.getSeverity();
    }

    @Override
    public void log(Level level, ResourceBundle bundle, String message, Throwable thrown) {
        if (!isLoggable(level)) return;
        final int severity = level.getSeverity() >= Level.ERROR.getSeverity() ? 2 : level.getSeverity() >= Level.WARNING.getSeverity() ? 1 : 0;
        bridge.log(severity, plugin + ": " + message + (thrown == null ? "" : " (" + thrown + ")"));
    }

    @Override
    public void log(Level level, ResourceBundle bundle, String format, Object... params) {
        log(level, bundle, params == null || params.length == 0 ? format : MessageFormat.format(format, params), (Throwable) null);
    }
}
