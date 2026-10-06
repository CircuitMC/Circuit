// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.bridge;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;

/**
 * Bridge protocol version 1: frames of a big-endian 32-bit length (1 to
 * 1 MiB) and a payload of one type byte followed by fields in
 * DataOutputStream encoding (big-endian numbers, modified UTF-8 strings).
 * docs/JAVA_BRIDGE.md describes the messages.
 */
final class Protocol {
    static final int VERSION = 1;
    static final int MAX_FRAME = 1 << 20;

    // From the bridge to the server.
    static final int HELLO = 1, LOG = 2, REQUEST = 3, TICK_DONE = 4, COMMAND_REPLY = 5, READY = 6;
    // From the server to the bridge.
    static final int WELCOME = 101, RESPONSE = 102, TICK = 103, COMMAND = 104, SHUTDOWN = 105;
    // Request operations.
    static final int OP_TIME = 1, OP_BLOCK_AT = 2, OP_ITEMS_IN_CHUNK = 3, OP_SET_BLOCK = 4, OP_USE_BLOCK = 5,
            OP_REGISTER_COMMAND = 6, OP_SUBSCRIBE_TICKS = 7;
    // Response status.
    static final int OK = 0, DENIED = 1, INVALID = 2, UNAVAILABLE = 3;

    private Protocol() {}

    /** A payload under construction. */
    static final class Message {
        private final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        final DataOutputStream out = new DataOutputStream(bytes);

        Message(int type) {
            bytes.write(type);
        }

        byte[] payload() {
            return bytes.toByteArray();
        }
    }

    static void writeFrame(DataOutputStream out, byte[] payload) throws IOException {
        if (payload.length < 1 || payload.length > MAX_FRAME) throw new IOException("frame size " + payload.length);
        out.writeInt(payload.length);
        out.write(payload);
        out.flush();
    }

    /** Reads one payload; fails on a size outside the limits. */
    static byte[] readFrame(DataInputStream in) throws IOException {
        final int length;
        try {
            length = in.readInt();
        } catch (EOFException e) {
            return null;
        }
        if (length < 1 || length > MAX_FRAME) throw new IOException("frame size " + length);
        final byte[] payload = new byte[length];
        in.readFully(payload);
        return payload;
    }
}
