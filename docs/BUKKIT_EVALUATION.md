# Bukkit, Spigot and Paper compatibility: evaluation

## Question

Could Circuit run plugins written for the Bukkit API, or for Spigot and Paper, which extend it? This note evaluates that as a separate project. It is not a plan of record, and Circuit makes no claim of Bukkit compatibility.

## What the API assumes

The Bukkit API is a Java API that plugins call directly, from the main server thread, against live game objects:

- **Synchronous world access.** `World.getBlockAt`, `Block.setType`, `Entity.teleport` and similar methods read and change the world immediately. Event handlers can cancel or modify an action before it happens (`BlockPlaceEvent`, `BlockRedstoneEvent`, `EntityDamageEvent`). Many plugins depend on the change being visible on the next line of code.
- **One main thread.** Most methods may only be called from the main thread. Plugins rely on the ordering this gives, and on scheduler tasks (`runTask`, `runTaskTimer`) running between ticks on that thread.
- **Object identity.** Plugins keep `Player`, `Entity`, `Block`, `Inventory` and `ItemStack` objects. They compare them, store them in maps and expect them to stay valid while the underlying thing exists.
- **Vanilla content and mechanics.** The API exposes the full vanilla content (every material, entity type, inventory, recipe and game rule), survival gameplay (damage, health, hunger) and inventory screens.
- **Implementation access.** Many popular plugins go below the API, to server internals (NMS) or Paper-specific classes, and break across server versions even on the reference implementations.

## Where Circuit differs

| Bukkit assumption | Circuit |
| --- | --- |
| Synchronous reads and writes | The world is owned by region jobs during a tick. Extensions read a published, versioned view and submit commands that apply at the next tick, in admission order, and are logged for replay. |
| Cancellable pre-action events | Player actions become commands before the tick. A pre-action hook would have to run at admission, before the rules have computed effects such as updates, drops and power. Effects inside the rules are not interceptable. |
| One main thread | The engine runs regions in parallel. Plugin callbacks run on the control thread between ticks, never inside region jobs. |
| Live object references | Plugins see ids and values: positions, entity ids, item stacks. |
| Full vanilla content | The reference rules model a redstone subset; the server is creative-only, without survival mechanics or container screens. |
| Java in-process | Java plugins run in a separate JVM over a bounded local protocol, so that a slow or failing JVM cannot stall the tick. |

## Options

1. **Full Bukkit/Paper API in the bridge JVM.** Implement the `org.bukkit` interfaces over the bridge protocol. Every synchronous call would become a round trip to the host and could not see changes before the next tick. This breaks the most common plugin patterns: reading a block just set, or cancelling an event. It also requires the full vanilla content model. Not viable.
2. **A compatible subset for read-only and command-style plugins.** Examples are chat commands, tab lists, scoreboards and periodic tasks that issue commands. Such a subset could map onto Circuit's queries, commands and subscriptions. Each supported class and method would need to be listed, with documented differences (deferred writes, no cancellation). It would help only plugins written against that subset, which in practice means plugins written for Circuit.
3. **The Circuit Java API.** Expose Circuit's own model to Java plugins: queries of the published view, commands with admission ids, tick-boundary events, technical tools. This is the M6 Java bridge.

## Recommendation

Do option 3 and do not claim Bukkit compatibility. Revisit option 2 only if there is concrete demand for specific plugins. Before any work, the evaluation would need:
- a list of the target plugins and of the API surface they use, measured by static analysis of their jars;
- a compatibility matrix per class and method, separate from Circuit's protocol, content and behavior matrices;
- tests that run those plugins and compare their observable behavior with Paper on the same version.

A claim of "Bukkit support" would mislead operators, because most real plugins depend on synchronous semantics or internals that Circuit does not provide.
