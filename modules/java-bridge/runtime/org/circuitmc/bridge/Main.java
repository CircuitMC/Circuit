// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.bridge;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.URL;
import java.net.URLClassLoader;
import java.net.UnixDomainSocketAddress;
import java.nio.channels.Channels;
import java.nio.channels.SocketChannel;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.ServiceLoader;
import org.circuitmc.spi.CircuitPlugin;

/**
 * Entry point of the bridge JVM, started by the server:
 * {@code --socket <path> [--plugin-class <name>]... [--plugins <directory>]}.
 * The connection token comes from the CIRCUIT_BRIDGE_TOKEN environment
 * variable, which only processes of the same user can read.
 */
public final class Main {
    private Main() {}

    public static void main(String[] args) throws Exception {
        String socket = null;
        final List<String> classes = new ArrayList<>();
        Path directory = null;
        for (int i = 0; i < args.length; ++i) {
            switch (args[i]) {
                case "--socket" -> socket = args[++i];
                case "--plugin-class" -> classes.add(args[++i]);
                case "--plugins" -> directory = Path.of(args[++i]);
                default -> throw new IllegalArgumentException("unknown argument " + args[i]);
            }
        }
        final String token = System.getenv("CIRCUIT_BRIDGE_TOKEN");
        if (socket == null || token == null) {
            System.err.println("circuit bridge: --socket and CIRCUIT_BRIDGE_TOKEN are required");
            System.exit(2);
        }
        final List<CircuitPlugin> plugins = new ArrayList<>();
        for (final String name : classes)
            plugins.add(Class.forName(name).asSubclass(CircuitPlugin.class).getDeclaredConstructor().newInstance());
        if (directory != null) plugins.addAll(discover(directory));
        try (SocketChannel channel = SocketChannel.open(StandardProtocolFamily.UNIX)) {
            channel.connect(UnixDomainSocketAddress.of(socket));
            final var in = new DataInputStream(new BufferedInputStream(Channels.newInputStream(channel)));
            final var out = new DataOutputStream(new BufferedOutputStream(Channels.newOutputStream(channel)));
            final Bridge bridge = new Bridge(in, out);
            bridge.handshake(token);
            // Startup runs on the plugin thread while this thread reads answers.
            bridge.start(plugins);
            bridge.run();
        }
        System.exit(0);
    }

    /** Plugins of every jar in a directory, each jar with its own class loader. */
    private static List<CircuitPlugin> discover(Path directory) throws IOException {
        final List<CircuitPlugin> found = new ArrayList<>();
        try (DirectoryStream<Path> jars = Files.newDirectoryStream(directory, "*.jar")) {
            final List<Path> sorted = new ArrayList<>();
            jars.forEach(sorted::add);
            sorted.sort(null);
            for (final Path jar : sorted) {
                final var loader = new URLClassLoader(new URL[] {jar.toUri().toURL()}, Main.class.getClassLoader());
                for (final CircuitPlugin p : ServiceLoader.load(CircuitPlugin.class, loader)) found.add(p);
            }
        }
        return found;
    }
}
