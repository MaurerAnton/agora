package android.util;

public class Base64 {
    public static final int DEFAULT = 0;
    public static final int NO_WRAP = 2;
    public static final int NO_PADDING = 4;
    public static final int URL_SAFE = 8;

    public static String encodeToString(byte[] input, int flags) {
        boolean urlSafe = (flags & URL_SAFE) != 0;
        boolean noPadding = (flags & NO_PADDING) != 0;
        java.util.Base64.Encoder encoder = java.util.Base64.getEncoder();
        if (noPadding) encoder = encoder.withoutPadding();
        String encoded = encoder.encodeToString(input);
        if (urlSafe) {
            encoded = encoded.replace('+', '-').replace('/', '_');
        }
        return encoded;
    }

    public static byte[] decode(String input, int flags) {
        boolean urlSafe = (flags & URL_SAFE) != 0;
        String s = input.replace('-', '+').replace('_', '/');
        int paddingNeeded = (4 - s.length() % 4) % 4;
        String padded = s + "=".repeat(paddingNeeded);
        return java.util.Base64.getDecoder().decode(padded);
    }
}
