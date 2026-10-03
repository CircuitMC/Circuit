# Circuit

Circuit is an experimental Minecraft Java Edition server written in C++23, built with xmake and standalone Asio. Its design uses chunk ownership, explicit cross-chunk coordination, and versioned game rules to explore parallel simulation without losing observable game behavior.

## Status

The project is at the framework stage. The prototype includes a task executor, synthetic simulation tests, native shared-library plugins, and module interfaces. Minecraft client login and playable worlds are not yet supported.

## Project goals

- Deterministic chunk-level scheduling with a single writer for each mutable owner.
- One complete game-version bundle selected at startup.
- Native C++ plugins and mods, with a separate bridge for Circuit Java plugins.
- Reliable world storage, vanilla import/export, and measured compression improvements.
- Server-authoritative action validation and tools for technical Minecraft.

See [the roadmap](TODO.md) for development milestones and planned capabilities.

## Source availability

This public branch contains the project overview and roadmap. Implementation source and detailed development documents are maintained locally and are not included in a public checkout. The implementation is intended for release under AGPL-3.0-only, with an optional alternative commercial license from the relevant rights holders.

Circuit is an independent project and is not affiliated with Mojang or Microsoft.
