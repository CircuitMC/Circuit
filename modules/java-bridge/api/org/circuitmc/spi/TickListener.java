// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.spi;

import java.util.List;

/** Receives committed ticks. */
@FunctionalInterface
public interface TickListener {
    void onTick(Tick tick) throws Exception;

    /** A committed tick and the block changes it made, in order. */
    record Tick(long number, List<BlockChange> changes) {}

    record BlockChange(int x, int y, int z, String before, String after) {}
}
