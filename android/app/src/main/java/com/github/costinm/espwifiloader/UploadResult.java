package com.github.costinm.espwifiloader;

/** Final line of an upload: "OK ..." or "ERR code reason". */
public final class UploadResult {
    public final boolean ok;
    public final String line;

    public UploadResult(boolean ok, String line) {
        this.ok = ok;
        this.line = line;
    }
}
