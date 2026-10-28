package dev.drawdo.datafriend;

import android.content.Context;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** Datafriend platform adapter. No method channel and no private bytes in Dart. */
public final class IdentityStore {
    private IdentityStore() {}
    public static void install(Context context) {
        System.loadLibrary("datafriend");
        initialize(context.getApplicationContext());
    }
    private static native void initialize(Context context);

    // Called only by native Datafriend. Returns a seed which JNI immediately
    // consumes and wipes; does not retain key material in Java static fields.
    private static synchronized byte[] loadOrCreate(Context context, String alias, byte[] candidate)
            throws Exception {
        try {
            byte[] aad = ("datafriend-ed25519-v1:" + alias).getBytes(StandardCharsets.UTF_8);
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(aad);
            StringBuilder name = new StringBuilder();
            for (byte b : digest) name.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            File directory = new File(context.getNoBackupFilesDir(), "datafriend-identities");
            if (!directory.isDirectory() && !directory.mkdirs())
                throw new java.io.IOException("Cannot create identity directory");
            // Serialize across processes as well as threads. The lock file and
            // ciphertext live in private, explicitly non-backed-up app storage.
            try (RandomAccessFile lockFile = new RandomAccessFile(new File(directory, name + ".lock"), "rw");
                 FileLock lock = lockFile.getChannel().lock()) {
                File file = new File(directory, name + ".key");
                AtomicFile atomic = new AtomicFile(file);
                String keyAlias = "dev.drawdo.datafriend.wrap.v1." + name;
                KeyStore store = KeyStore.getInstance("AndroidKeyStore");
                store.load(null);
                SecretKey key = (SecretKey) store.getKey(keyAlias, null);
                // AtomicFile may have a recoverable backup after a crash.
                boolean exists = file.exists() || new File(file + ".bak").exists();
                if (exists) {
                    if (key == null) throw new java.security.KeyStoreException("Identity wrapping key missing");
                    byte[] record;
                    try (java.io.FileInputStream in = atomic.openRead()) {
                        record = new byte[61];
                        int offset = 0, n;
                        while (offset < record.length && (n = in.read(record, offset, record.length - offset)) > 0)
                            offset += n;
                        if (offset != 61 || in.read() != -1 || record[0] != 1)
                            throw new java.io.IOException("Invalid identity record");
                    }
                    Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
                    cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, record, 1, 12));
                    cipher.updateAAD(aad);
                    byte[] seed = cipher.doFinal(record, 13, 48);
                    if (seed.length != 32) {
                        Arrays.fill(seed, (byte) 0);
                        throw new java.io.IOException("Invalid identity seed");
                    }
                    return seed;
                }
                if (key == null) {
                    KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
                    generator.init(new KeyGenParameterSpec.Builder(keyAlias,
                            KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                            .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                            .setRandomizedEncryptionRequired(true).build());
                    key = generator.generateKey();
                } else {
                    // A lost encrypted record must not silently become a new
                    // namespace. Also fails closed after a crash during creation.
                    throw new java.io.IOException("Identity record missing; choose a new alias explicitly");
                }
                Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
                cipher.init(Cipher.ENCRYPT_MODE, key);
                cipher.updateAAD(aad);
                byte[] iv = cipher.getIV();
                if (iv.length != 12) throw new java.security.GeneralSecurityException("Invalid identity IV");
                byte[] encrypted = cipher.doFinal(candidate);
                FileOutputStream output = atomic.startWrite();
                try {
                    output.write(1); output.write(iv); output.write(encrypted);
                    output.getFD().sync();
                    atomic.finishWrite(output);
                    // AtomicFile can log some filesystem failures instead of
                    // throwing. Verify the durable record before publishing a key.
                    byte[] expected = new byte[61];
                    expected[0] = 1;
                    System.arraycopy(iv, 0, expected, 1, 12);
                    System.arraycopy(encrypted, 0, expected, 13, 48);
                    if (!Arrays.equals(atomic.readFully(), expected))
                        throw new java.io.IOException("Identity write verification failed");
                } catch (Exception failure) {
                    atomic.failWrite(output);
                    throw failure;
                }
                return candidate.clone();
            }
        } finally {
            Arrays.fill(candidate, (byte) 0);
        }
    }
}
