// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.example;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import org.circuitmc.spi.CircuitPlugin;
import org.circuitmc.spi.PluginContext;
import org.circuitmc.spi.TickListener;

/**
 * Example plugin. {@code /pulse <x> <y> <z> [ticks]} places a redstone block for
 * a number of ticks and then removes it; {@code /block <x> <y> <z>} reports the
 * block at a position. It also counts the block changes it observes.
 */
public final class ExamplePlugin implements CircuitPlugin {
    private PluginContext context;
    private final List<long[]> pulses = new ArrayList<>(); // x, y, z, end tick
    private long changes;

    @Override
    public void onLoad(PluginContext value) {
        context = Objects.requireNonNull(value);
    }

    @Override
    public void onEnable() {
        context.registerCommand("pulse", "<x> <y> <z> [ticks]", (player, args) -> {
            if (args.size() < 3) return "Usage: /pulse <x> <y> <z> [ticks]";
            final int x = Integer.parseInt(args.get(0)), y = Integer.parseInt(args.get(1)), z = Integer.parseInt(args.get(2));
            final long ticks = args.size() > 3 ? Long.parseLong(args.get(3)) : 10;
            final long now = context.world().time().get();
            context.world().setBlock(x, y, z, "minecraft:redstone_block").get();
            pulses.add(new long[] {x, y, z, now + ticks});
            return "Pulse at " + x + " " + y + " " + z + " for " + ticks + " ticks";
        });
        context.registerCommand("block", "<x> <y> <z>", (player, args) -> {
            if (args.size() != 3) return "Usage: /block <x> <y> <z>";
            return context.world().blockAt(Integer.parseInt(args.get(0)), Integer.parseInt(args.get(1)), Integer.parseInt(args.get(2))).get();
        });
        context.onTick(this::tick);
        context.logger().log(System.Logger.Level.INFO, "enabled");
    }

    private void tick(TickListener.Tick tick) {
        changes += tick.changes().size();
        pulses.removeIf(p -> {
            if (tick.number() < p[3]) return false;
            context.world().setBlock((int) p[0], (int) p[1], (int) p[2], "minecraft:air");
            return true;
        });
        if (tick.number() % 200 == 0)
            context.logger().log(System.Logger.Level.INFO, "observed " + changes + " block changes so far");
    }

    @Override
    public void onDisable() {
        context.logger().log(System.Logger.Level.INFO, "disabled after observing " + changes + " block changes");
    }
}
