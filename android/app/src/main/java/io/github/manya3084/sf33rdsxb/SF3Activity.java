package io.github.manya3084.sf33rdsxb;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import org.libsdl.app.SDLActivity;

/* SDL's activity, with the game's libraries: SDL3, OpenAL Soft, and the
 * game itself (libmain.so, whose SDL_main SDL calls). */
public class SF3Activity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "openal", "main" };
    }

    /* The last run's log on the clipboard, to paste into a message
     * (src/port/main.c). Called from the game's thread: the clipboard is
     * the UI thread's. */
    public void copyText(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                ClipboardManager clip = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                if (clip != null) {
                    clip.setPrimaryClip(ClipData.newPlainText("3rd Strike log", text));
                }
            }
        });
    }

    /* SCREEN ADJUST's ORIENTATION (src/port/display.c): landscape or
     * portrait, held there whichever way the phone is turned. */
    public void setFixedOrientation(final boolean portrait) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                setRequestedOrientation(portrait ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                                                 : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            }
        });
    }

    /* The file picker's document as a descriptor the game reads the disc
     * through (src/port/setup.c); -1 when it cannot be opened. */
    public int openDocument(String uri) {
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }
}
