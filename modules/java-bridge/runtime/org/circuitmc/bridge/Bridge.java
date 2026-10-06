// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.bridge;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import org.circuitmc.spi.CircuitException;
import org.circuitmc.spi.CircuitPlugin;
import org.circuitmc.spi.CommandHandler;
import org.circuitmc.spi.PluginContext;
import org.circuitmc.spi.TickListener;
import org.circuitmc.spi.World;

/**
 * The bridge side of a connection: plugin contexts, pending requests and the
 * plugin thread. A reader thread decodes frames from the server; plugin code
 * runs only on the single plugin thread.
 */
final class Bridge {
    private interface Decoder<T> {
        T decode(DataInputStream in) throws IOException;
    }

    private record Pending(CompletableFuture<Object> future, Decoder<?> decoder) {}

    private final DataInputStream in;
    private final DataOutputStream out;
    private final ExecutorService pluginThread = Executors.newSingleThreadExecutor(r -> {
        final Thread t = new Thread(r, "circuit-plugins");
        t.setDaemon(true);
        return t;
    });
    private final AtomicLong nextRequest = new AtomicLong(1);
    private final Map<Long, Pending> pending = new ConcurrentHashMap<>();
    private final Map<String, CommandHandler> commands = new ConcurrentHashMap<>();
    private final List<TickListener> tickListeners = new ArrayList<>();
    private final List<Loaded> plugins = new ArrayList<>();
    private volatile boolean subscribed;
    private int permissions;

    private record Loaded(String id, CircuitPlugin plugin) {}

    Bridge(DataInputStream in, DataOutputStream out) {
        this.in = in;
        this.out = out;
    }

    void send(byte[] payload) {
        synchronized (out) {
            try {
                Protocol.writeFrame(out, payload);
            } catch (IOException e) {
                throw new CircuitException(CircuitException.Reason.UNAVAILABLE, "the server connection is closed");
            }
        }
    }

    void log(int level, String text) {
        final var m = new Protocol.Message(Protocol.LOG);
        try {
            m.out.writeByte(level);
            m.out.writeUTF(text.length() > 8000 ? text.substring(0, 8000) : text);
        } catch (IOException e) {
            throw new IllegalStateException(e);
        }
        send(m.payload());
    }

    /** Authenticates and waits for the server's welcome. */
    void handshake(String token) throws IOException {
        final var hello = new Protocol.Message(Protocol.HELLO);
        hello.out.writeInt(Protocol.VERSION);
        hello.out.writeUTF(token);
        send(hello.payload());
        final byte[] frame = Protocol.readFrame(in);
        if (frame == null) throw new IOException("the server closed the connection during the handshake");
        final var welcome = new DataInputStream(new ByteArrayInputStream(frame));
        if (welcome.readUnsignedByte() != Protocol.WELCOME || welcome.readInt() != Protocol.VERSION)
            throw new IOException("unexpected handshake answer");
        welcome.readUTF(); // bundle id
        permissions = welcome.readInt();
    }

    /** Loads and enables the plugins on the plugin thread. */
    void start(List<CircuitPlugin> instances) throws Exception {
        pluginThread.submit(() -> {
            for (final CircuitPlugin p : instances) {
                final String id = p.getClass().getName();
                try {
                    p.onLoad(new Context(id));
                    p.onEnable();
                    plugins.add(new Loaded(id, p));
                } catch (Exception | LinkageError e) {
                    log(2, id + " failed to start: " + e);
                }
            }
            final var ready = new Protocol.Message(Protocol.READY);
            ready.out.writeInt(plugins.size());
            for (final Loaded l : plugins) ready.out.writeUTF(l.id());
            send(ready.payload());
            return null;
        }).get();
    }

    /** Reads frames until the server stops the bridge or the connection ends. */
    void run() throws IOException {
        while (true) {
            final byte[] frame = Protocol.readFrame(in);
            if (frame == null) break;
            final var msg = new DataInputStream(new ByteArrayInputStream(frame));
            final int type = msg.readUnsignedByte();
            if (type == Protocol.RESPONSE) {
                response(msg);
            } else if (type == Protocol.TICK) {
                tick(msg);
            } else if (type == Protocol.COMMAND) {
                command(msg);
            } else if (type == Protocol.SHUTDOWN) {
                break;
            } else {
                throw new IOException("unknown message type " + type);
            }
        }
        stop();
    }

    void stop() {
        try {
            pluginThread.submit(() -> {
                for (int i = plugins.size() - 1; i >= 0; --i) {
                    try {
                        plugins.get(i).plugin().onDisable();
                    } catch (Exception | LinkageError e) {
                        System.err.println(plugins.get(i).id() + " failed to stop: " + e);
                    }
                }
                plugins.clear();
            }).get(5, TimeUnit.SECONDS);
        } catch (Exception e) {
            System.err.println("plugins did not stop in time: " + e);
        }
        pluginThread.shutdownNow();
        for (final Pending p : pending.values())
            p.future().completeExceptionally(new CircuitException(CircuitException.Reason.UNAVAILABLE, "the bridge stopped"));
    }

    private void response(DataInputStream msg) throws IOException {
        final long id = msg.readLong();
        final int status = msg.readUnsignedByte();
        final Pending p = pending.remove(id);
        if (p == null) throw new IOException("response to unknown request " + id);
        if (status == Protocol.OK) {
            p.future().complete(p.decoder().decode(msg));
        } else {
            final var reason = status == Protocol.DENIED ? CircuitException.Reason.DENIED
                    : status == Protocol.INVALID ? CircuitException.Reason.INVALID : CircuitException.Reason.UNAVAILABLE;
            p.future().completeExceptionally(new CircuitException(reason, msg.readUTF()));
        }
    }

    private void tick(DataInputStream msg) throws IOException {
        final long number = msg.readLong();
        final int n = msg.readInt();
        final List<TickListener.BlockChange> changes = new ArrayList<>(Math.min(n, 65536));
        for (int i = 0; i < n; ++i)
            changes.add(new TickListener.BlockChange(msg.readInt(), msg.readInt(), msg.readInt(), msg.readUTF(), msg.readUTF()));
        final var event = new TickListener.Tick(number, List.copyOf(changes));
        pluginThread.execute(() -> {
            for (final TickListener l : tickListeners) {
                try {
                    l.onTick(event);
                } catch (Exception | LinkageError e) {
                    log(2, "tick listener failed: " + e);
                }
            }
            final var done = new Protocol.Message(Protocol.TICK_DONE);
            try {
                done.out.writeLong(number);
            } catch (IOException e) {
                throw new IllegalStateException(e);
            }
            send(done.payload());
        });
    }

    private void command(DataInputStream msg) throws IOException {
        final long invocation = msg.readLong();
        final String player = msg.readUTF();
        final String name = msg.readUTF();
        final int argc = msg.readInt();
        if (argc < 0 || argc > 256) throw new IOException("invalid argument count");
        final List<String> args = new ArrayList<>(argc);
        for (int i = 0; i < argc; ++i) args.add(msg.readUTF());
        pluginThread.execute(() -> {
            String reply;
            final CommandHandler h = commands.get(name);
            try {
                reply = h == null ? "Unknown command" : h.run(player, List.copyOf(args));
            } catch (Exception | LinkageError e) {
                reply = "The command failed: " + e.getMessage();
                log(2, "command /" + name + " failed: " + e);
            }
            final var m = new Protocol.Message(Protocol.COMMAND_REPLY);
            try {
                m.out.writeLong(invocation);
                m.out.writeUTF(reply == null ? "" : reply);
            } catch (IOException e) {
                throw new IllegalStateException(e);
            }
            send(m.payload());
        });
    }

    @SuppressWarnings("unchecked")
    private <T> CompletableFuture<T> request(int op, Encoder fields, Decoder<T> decoder) {
        final long id = nextRequest.getAndIncrement();
        final var future = new CompletableFuture<Object>();
        pending.put(id, new Pending(future, decoder));
        final var m = new Protocol.Message(Protocol.REQUEST);
        try {
            m.out.writeLong(id);
            m.out.writeByte(op);
            fields.encode(m.out);
            send(m.payload());
        } catch (IOException | RuntimeException e) {
            pending.remove(id);
            future.completeExceptionally(e instanceof CircuitException ? e : new CircuitException(CircuitException.Reason.INVALID, e.toString()));
        }
        return (CompletableFuture<T>) (CompletableFuture<?>) future;
    }

    private interface Encoder {
        void encode(DataOutputStream out) throws IOException;
    }

    private final class Context implements PluginContext {
        private final String id;
        private final System.Logger logger;

        Context(String id) {
            this.id = id;
            this.logger = new BridgeLogger(Bridge.this, id);
        }

        @Override
        public String pluginId() {
            return id;
        }

        @Override
        public System.Logger logger() {
            return logger;
        }

        @Override
        public World world() {
            return worldAccess;
        }

        @Override
        public void registerCommand(String name, String usage, CommandHandler handler) {
            if (name == null || !name.matches("[a-z0-9_-]{1,32}")) throw new IllegalArgumentException("invalid command name");
            if (commands.putIfAbsent(name, handler) != null) throw new IllegalArgumentException("command /" + name + " is taken");
            request(Protocol.OP_REGISTER_COMMAND, out -> {
                out.writeUTF(name);
                out.writeUTF(usage == null ? "" : usage);
            }, in -> null).whenComplete((ok, error) -> {
                if (error != null) {
                    commands.remove(name);
                    log(1, id + ": command /" + name + " was refused: " + error.getMessage());
                }
            });
        }

        @Override
        public void onTick(TickListener listener) {
            tickListeners.add(listener);
            if (!subscribed) {
                subscribed = true;
                request(Protocol.OP_SUBSCRIBE_TICKS, out -> {}, in -> null).whenComplete((ok, error) -> {
                    if (error != null) log(1, id + ": tick subscription refused: " + error.getMessage());
                });
            }
        }
    }

    private final World worldAccess = new World() {
        @Override
        public CompletableFuture<Long> time() {
            return request(Protocol.OP_TIME, out -> {}, DataInputStream::readLong);
        }

        @Override
        public CompletableFuture<String> blockAt(int x, int y, int z) {
            return request(Protocol.OP_BLOCK_AT, out -> {
                out.writeInt(x);
                out.writeInt(y);
                out.writeInt(z);
            }, in -> in.readUTF());
        }

        @Override
        public CompletableFuture<List<ItemEntity>> itemsInChunk(int chunkX, int chunkZ) {
            return request(Protocol.OP_ITEMS_IN_CHUNK, out -> {
                out.writeInt(chunkX);
                out.writeInt(chunkZ);
            }, in -> {
                final int n = in.readInt();
                final List<ItemEntity> items = new ArrayList<>(Math.min(n, 65536));
                for (int i = 0; i < n; ++i)
                    items.add(new ItemEntity(in.readLong(), in.readDouble(), in.readDouble(), in.readDouble(), in.readUTF(), in.readInt()));
                return List.copyOf(items);
            });
        }

        @Override
        public CompletableFuture<Long> setBlock(int x, int y, int z, String state) {
            return request(Protocol.OP_SET_BLOCK, out -> {
                out.writeInt(x);
                out.writeInt(y);
                out.writeInt(z);
                out.writeUTF(state);
            }, DataInputStream::readLong);
        }

        @Override
        public CompletableFuture<Long> useBlock(int x, int y, int z) {
            return request(Protocol.OP_USE_BLOCK, out -> {
                out.writeInt(x);
                out.writeInt(y);
                out.writeInt(z);
            }, DataInputStream::readLong);
        }
    };
}
