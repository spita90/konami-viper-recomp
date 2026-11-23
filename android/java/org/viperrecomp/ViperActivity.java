package org.viperrecomp;

import android.os.Build;
import android.os.Bundle;
import android.view.DisplayCutout;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/** SDL's activity, with what runtime/platform_android.c asks of it through JNI. */
public class ViperActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // the picture also beside a camera cutout; the touch controls keep out of it (safeInsets)
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
    }

    /** the display cutout's insets in pixels: left, top, right, bottom */
    public int[] safeInsets() {
        int[] r = new int[4];
        if (Build.VERSION.SDK_INT >= 28) {
            WindowInsets w = getWindow().getDecorView().getRootWindowInsets();
            DisplayCutout c = w != null ? w.getDisplayCutout() : null;
            if (c != null) {
                r[0] = c.getSafeInsetLeft();
                r[1] = c.getSafeInsetTop();
                r[2] = c.getSafeInsetRight();
                r[3] = c.getSafeInsetBottom();
            }
        }
        return r;
    }

    /** a light tap for a touch button, as the system's keyboard gives (its haptics setting applies) */
    public void hapticTap() {
        final View v = getWindow().getDecorView();
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            }
        });
    }
}
