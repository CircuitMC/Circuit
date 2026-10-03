// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.spi;

/** No mutable world access or synchronous chunk-thread callbacks are exposed. */
public interface PluginContext {
    int SPI_VERSION = 1;
    String pluginId();
    System.Logger logger();
}
