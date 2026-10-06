// SPDX-License-Identifier: AGPL-3.0-only
package org.circuitmc.spi;

/** A refused or failed request. */
public final class CircuitException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    public enum Reason { DENIED, INVALID, UNAVAILABLE }

    private final Reason reason;

    public CircuitException(Reason reason, String message) {
        super(message);
        this.reason = reason;
    }

    public Reason reason() {
        return reason;
    }
}
