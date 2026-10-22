package io.github.manya3084.sf33rdsxb;

import android.content.Intent;
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

    /* The last run's log, to send wherever the player likes (src/port/main.c). */
    public void shareText(String text) {
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_SUBJECT, "3rd Strike log");
        send.putExtra(Intent.EXTRA_TEXT, text);
        startActivity(Intent.createChooser(send, "Share the log"));
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
