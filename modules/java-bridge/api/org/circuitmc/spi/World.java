// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.spi;

import java.util.List;
import java.util.concurrent.CompletableFuture;

/**
 * Controlled world access. Every call is answered by the server at the next
 * tick boundary. A failed call completes exceptionally with a
 * {@link CircuitException}. Block states use the game's notation, for example
 * {@code minecraft:repeater[delay=2,facing=west,locked=false,powered=false]}.
 */
public interface World {
    /** Ticks the world has run. */
    CompletableFuture<Long> time();

    /** The block state at a position, as published after the last tick. */
    CompletableFuture<String> blockAt(int x, int y, int z);

    /** Item entities in a chunk column. */
    CompletableFuture<List<ItemEntity>> itemsInChunk(int chunkX, int chunkZ);

    /**
     * Places a block state at the start of a later tick. Completes with the
     * command's admission number once it is admitted, not when it is applied.
     */
    CompletableFuture<Long> setBlock(int x, int y, int z, String state);

    /** Uses a block, as a player would (for example, flips a lever). */
    CompletableFuture<Long> useBlock(int x, int y, int z);

    record ItemEntity(long id, double x, double y, double z, String item, int count) {}
}
