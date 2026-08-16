package io.nava.whisper_destilado;

import android.Manifest;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;

import io.nava.appshell.AppShellActivity;

/**
 * The app's activity: app_shell's AppShellActivity (NativeActivity plus the
 * IME/clipboard JNI half) with the one thing only this app knows — its own
 * library name — and a one-shot RECORD_AUDIO grant request.
 *
 * NativeActivity dlopens the library from native code, which never registers
 * it with the JVM, so the static block is what makes the AppShellActivity
 * native methods resolve. Without it every clipboard/IME call throws
 * UnsatisfiedLinkError and quietly no-ops. See app_shell/CLAUDE.md.
 */
public class MainActivity extends AppShellActivity {
    static {
        System.loadLibrary("whisper_destilado");
    }

    private static final int REQ_RECORD_AUDIO = 1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // AAudio returns an error unless RECORD_AUDIO was granted. Ask once,
        // up front; the native side reports a Spanish error if the user
        // denies and later tries to record.
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO},
                    REQ_RECORD_AUDIO);
        }
    }
}
