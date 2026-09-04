package foundation.sm64;

import android.os.Bundle;
import android.view.KeyEvent;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import org.libsdl.app.SDLActivity;

/**
 * SDL3 shell around the sm64_foundation native library.
 *
 * SDLActivity loads libSDL3.so plus libsm64_foundation.so and resolves SDL_main
 * from the latter; the game loop itself is unchanged from the desktop build.
 */
public class SM64Activity extends SDLActivity {
    private OnBackInvokedCallback backCallback;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        backCallback = new OnBackInvokedCallback() {
            @Override
            public void onBackInvoked() {
                SDLActivity.nativeSendQuit();
            }
        };
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                backCallback
        );
    }

    @Override
    protected void onDestroy() {
        if (backCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
            backCallback = null;
        }
        super.onDestroy();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "sm64_foundation"
        };
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP)
                SDLActivity.nativeSendQuit();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
}
