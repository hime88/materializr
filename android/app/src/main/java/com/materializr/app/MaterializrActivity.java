package com.materializr.app;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.View;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

// Materializr's Android entry activity. SDLActivity does the heavy lifting:
// it creates the GL surface, loads the native libraries below, and calls
// SDL_main (defined in android_main.cpp) on its own thread.
public class MaterializrActivity extends SDLActivity {

    private static MaterializrActivity sInstance;

    // ---- Storage Access Framework + share bridge -----------------------------
    // The native file layer (FileDialogs.cpp) drives these via JNI and polls
    // pollFileResult() each frame for the async picker result. SAF gives a
    // content:// URI; since OCCT reads/writes plain paths, we copy through a
    // cache temp file: open = copy URI->temp then hand the temp path to native;
    // save = native writes a temp then we copy temp->URI.
    private static final int REQ_OPEN = 0xF11E;
    private static final int REQ_SAVE = 0xF12E;

    private static volatile boolean sResultReady = false;
    private static volatile String  sResultValue = "";   // open: temp path; save: "ok"; cancel: ""
    private Uri mPendingSaveUri;                          // destination chosen for a save
    private static volatile String sLastDocUri  = "";    // persisted URI of the last open/save
    private static volatile String sLastDocName = "";    // its display name (for Open Recent)

    // Native -> Java entry points (called from FileDialogs.cpp via JNI) ---------

    // Launch the system open picker. mimeCsv is comma-separated MIME types (or
    // "*/*"). The result arrives via onActivityResult -> pollFileResult().
    public static void nativeOpenDocument(String mimeCsv) {
        final MaterializrActivity a = sInstance;
        if (a == null) return;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            applyMimes(i, mimeCsv);
            // Request a *persistable* read grant so the picked document can be
            // re-opened later from the Open Recent list.
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                     | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try { a.startActivityForResult(i, REQ_OPEN); }
            catch (Exception e) { signal(""); }
        });
    }

    // Launch the system "save as" picker with a suggested name + MIME.
    public static void nativeCreateDocument(String name, String mime) {
        final MaterializrActivity a = sInstance;
        if (a == null) return;
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType((mime == null || mime.isEmpty()) ? "application/octet-stream" : mime);
            i.putExtra(Intent.EXTRA_TITLE, name);
            // Persistable read+write so a saved project lands in Open Recent too.
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                     | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                     | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try { a.startActivityForResult(i, REQ_SAVE); }
            catch (Exception e) { signal(""); }
        });
    }

    // After native has written tempPath, copy it into the save destination the
    // user picked. Returns true on success.
    public static boolean nativeCommitSave(String tempPath) {
        MaterializrActivity a = sInstance;
        if (a == null || a.mPendingSaveUri == null) return false;
        try (InputStream in = new FileInputStream(tempPath);
             OutputStream out = a.getContentResolver().openOutputStream(a.mPendingSaveUri, "w")) {
            copy(in, out);
            return true;
        } catch (Exception e) {
            return false;
        } finally {
            a.mPendingSaveUri = null;
            new File(tempPath).delete();
        }
    }

    // Share a just-written file via the system share sheet. Copies it into
    // <cache>/share (served by MaterializrFileProvider) and fires ACTION_SEND.
    public static void nativeShareFile(String path, String mime) {
        final MaterializrActivity a = sInstance;
        if (a == null) { android.util.Log.e("MZSHARE", "no activity"); return; }
        a.runOnUiThread(() -> {
            try {
                File src = new File(path);
                File dir = new File(a.getCacheDir(), "share");
                dir.mkdirs();
                File dst = new File(dir, src.getName());
                try (InputStream in = new FileInputStream(src);
                     OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
                Uri uri = Uri.parse("content://" + a.getPackageName() + ".fileprovider/" + dst.getName());
                Intent send = new Intent(Intent.ACTION_SEND);
                send.setType((mime == null || mime.isEmpty()) ? "application/octet-stream" : mime);
                send.putExtra(Intent.EXTRA_STREAM, uri);
                // ClipData + flag make the read grant propagate to the chosen app.
                send.setClipData(android.content.ClipData.newRawUri(dst.getName(), uri));
                send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                a.startActivity(Intent.createChooser(send, "Share " + dst.getName())
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION));
            } catch (Exception e) {
                android.util.Log.e("MZSHARE", "share failed", e);
            }
        });
    }

    // Native polls this each frame while a picker is open. Returns null while
    // pending, "" on cancel, or (open) the temp path / (save) "ok" when ready.
    public static String pollFileResult() {
        if (!sResultReady) return null;
        sResultReady = false;
        return sResultValue;
    }

    // Persisted-document URI accessors for the Open Recent list.
    public static String nativeLastDocUri()  { return sLastDocUri; }
    public static String nativeLastDocName() { return sLastDocName; }

    // Re-open a previously persisted document URI without a picker: copy it into
    // a cache temp and return that path ("" on failure — access revoked or the
    // file was deleted). Runs synchronously on the caller (native) thread.
    public static String nativeOpenUri(String uriString) {
        MaterializrActivity a = sInstance;
        if (a == null || uriString == null || uriString.isEmpty()) return "";
        try {
            Uri uri = Uri.parse(uriString);
            File dir = new File(a.getCacheDir(), "import");
            dir.mkdirs();
            File dst = new File(dir, a.queryName(uri));
            try (InputStream in = a.getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
            a.rememberDoc(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            return dst.getAbsolutePath();
        } catch (Exception e) {
            return "";
        }
    }

    private static void signal(String value) { sResultValue = value; sResultReady = true; }

    private static void applyMimes(Intent i, String mimeCsv) {
        if (mimeCsv == null || mimeCsv.isEmpty() || mimeCsv.equals("*/*")) {
            i.setType("*/*");
            return;
        }
        String[] mimes = mimeCsv.split(",");
        i.setType(mimes.length == 1 ? mimes[0] : "*/*");
        if (mimes.length > 1) i.putExtra(Intent.EXTRA_MIME_TYPES, mimes);
    }

    private static void copy(InputStream in, OutputStream out) throws Exception {
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        out.flush();
    }

    private String queryName(Uri uri) {
        String name = "import.bin";
        try (android.database.Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) { String n = c.getString(idx); if (n != null && !n.isEmpty()) name = n; }
            }
        } catch (Exception ignored) {}
        return new File(name).getName();
    }

    // Take a persistable permission on `uri` and record it as the last document
    // for the Open Recent list. Best-effort: some providers don't grant
    // persistable permissions, so recents may not be able to re-open those.
    private void rememberDoc(Uri uri, int modeFlags) {
        try {
            getContentResolver().takePersistableUriPermission(uri, modeFlags);
        } catch (Exception ignored) {}
        sLastDocUri = uri.toString();
        sLastDocName = queryName(uri);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_OPEN && requestCode != REQ_SAVE) return;
        Uri uri = (resultCode == Activity.RESULT_OK && data != null) ? data.getData() : null;
        if (uri == null) { signal(""); return; }  // cancelled
        if (requestCode == REQ_OPEN) {
            // Copy the chosen document into a cache temp file and hand back its path.
            try {
                File dir = new File(getCacheDir(), "import");
                dir.mkdirs();
                File dst = new File(dir, queryName(uri));
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(dst)) { copy(in, out); }
                rememberDoc(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                signal(dst.getAbsolutePath());
            } catch (Exception e) {
                signal("");
            }
        } else { // REQ_SAVE: remember the destination; native writes a temp then commits.
            mPendingSaveUri = uri;
            rememberDoc(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                           | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            signal("ok");
        }
    }

    // ---- Window mode (immersive on a bare tablet, windowed in a desktop dock) -

    private boolean isDesktopMode() {
        Configuration c = getResources().getConfiguration();
        boolean hwKeyboard = c.keyboard == Configuration.KEYBOARD_QWERTY
                && c.hardKeyboardHidden == Configuration.HARDKEYBOARDHIDDEN_NO;
        boolean multiWindow = Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
                && isInMultiWindowMode();
        return hwKeyboard || multiWindow;
    }

    private void applyWindowMode() {
        View dv = getWindow().getDecorView();
        if (!isDesktopMode()) {
            dv.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        } else {
            dv.setSystemUiVisibility(View.SYSTEM_UI_FLAG_VISIBLE);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;
        // No storage-permission prompt: file open/save use the system SAF picker
        // (per-URI access), and export sharing uses the FileProvider.
    }

    @Override
    protected void onDestroy() {
        if (sInstance == this) sInstance = null;
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyWindowMode();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        applyWindowMode();
    }

    @Override
    public void onMultiWindowModeChanged(boolean isInMultiWindowMode, Configuration newConfig) {
        super.onMultiWindowModeChanged(isInMultiWindowMode, newConfig);
        applyWindowMode();
    }

    @Override
    protected String[] getLibraries() {
        // libmain.so links the OCCT toolkits via DT_NEEDED, so the dynamic
        // loader pulls them (and libc++_shared) from the APK automatically.
        return new String[] {
            "SDL2",
            "main"
        };
    }
}
