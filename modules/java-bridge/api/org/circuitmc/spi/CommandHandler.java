// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.spi;

import java.util.List;

/** Handles a registered chat command. */
@FunctionalInterface
public interface CommandHandler {
    /** Returns the reply shown to the player. */
    String run(String player, List<String> arguments) throws Exception;
}
