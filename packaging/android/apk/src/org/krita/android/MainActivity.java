/*
 * This file is part of the KDE project
 * SPDX-FileCopyrightText: 2019 Sharaf Zaman <sharafzaz121@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

package org.krita.android;

import android.annotation.SuppressLint;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.ForegroundServiceStartNotAllowedException;
import android.app.ServiceStartNotAllowedException;
import android.content.Intent;
import android.content.res.Configuration;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewConfiguration;

import androidx.annotation.RequiresApi;

import java.util.List;

import org.libsdl.app.SDLAudioManager;
import org.qtproject.qt5.android.QtNative;
import org.qtproject.qt5.android.bindings.QtActivity;


public class MainActivity extends QtActivity {

    private static final String TAG = "krita.MainActivity";

    // Non-standard key codes reported by vendor stylus hardware for its gestures.
    private static final int STYLUS_GESTURE_KEY_FIRST = 194;
    private static final int STYLUS_GESTURE_KEY_LAST = 197;

    /** Vendor pen service client. Null when the service is unavailable. */
    private PenEngineClient mPenEngine = null;
    private boolean haveLibsLoaded = false;
    private boolean serviceStarted = false;
    private boolean inFullScreen = false;

    @Override
    @SuppressLint("MissingSuperCall")
    public void onCreate(Bundle savedInstanceState) {
        super.QT_ANDROID_DEFAULT_THEME = "DefaultTheme";

        // we have to do this before loading main()
        Intent i = getIntent();
        String uri = getUri(i);
        if (uri != null) {
            // this will be passed as a command line argument to main()
            i.putExtra("applicationArguments", uri);
        }

        SDLAudioManager.initialize();
        SDLAudioManager.setContext(this);
        SDLAudioManager.nativeSetupJNI();

        super.onCreate(savedInstanceState);

        // Start the vendor pen service so the ROM begins delivering barrel rotation and
        // the sliding / double tap gestures. Safe on devices without the service.
        mPenEngine = new PenEngineClient(this, new PenEngineClient.Listener() {
            @Override
            public void onStylusRotation(int degrees) {
                Log.i(TAG, "stylus rotation=" + degrees);
                JNIWrappers.stylusRotation(degrees);
            }

            @Override
            public void onTouchFilm(int code) {
                // Codes are ROM defined; sliding and double tap are also delivered as
                // keys, which is the path the gesture actions use.
                Log.i(TAG, "touch film code=" + code);
            }
        });
        mPenEngine.bind();

        Log.i(TAG, "TouchSlop: " + ViewConfiguration.get(this).getScaledTouchSlop());
        Log.i(TAG, "LibsLoaded");
        haveLibsLoaded = true;

    }

    @Override
    public void onStart() {
        super.onStart();

        // unlike onCreate where we did this before, this method is called several times throughout the
        // lifecycle of our app, but we intend to run this method only once (and in "Foreground").
        if (!serviceStarted) {
            serviceStarted  = true;
            // Full-screening the application here instead of after the main window is shown avoids
            // some ugly flicker as the Qt UI resizes itself.
            trySetFullScreen(true);
            // Keep the service started so in an unfortunate case where we're not allowed to start a
            // foreground service, we can try to continue without it.
            Intent docSaverServiceIntent = new Intent(this, DocumentSaverService.class);
            startService(docSaverServiceIntent);
        }
    }

    @Override
    protected void onNewIntent (Intent intent) {
        String uri = getUri(intent);
        if (uri != null) {
            JNIWrappers.openFileFromIntent(uri);
        }

        super.onNewIntent(intent);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if ((newConfig.uiMode & Configuration.UI_MODE_NIGHT_MASK) != Configuration.UI_MODE_NIGHT_UNDEFINED) {
            JNIWrappers.systemThemeChanged();
        }
    }

    private String getUri(Intent intent) {
        if (intent != null) {
            Uri fileUri = intent.getData();
            if (fileUri != null) {
                return fileUri.toString();
            }
        }
        return null;
    }

    @Override
    public void onPause() {
        super.onPause();
        // onPause() _is_ called when the app starts. If the native lib
        // isn't loaded, it crashes.
        if (haveLibsLoaded) {
            synchronized(this) {
                startServiceGeneric(DocumentSaverService.START_SAVING);
            }
        }
    }

    void startServiceGeneric(final String action) {
        Intent intent = new Intent(this, DocumentSaverService.class);
        intent.putExtra(action, true);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            startForegroundServiceS(intent);
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }
    }

    @RequiresApi(api = Build.VERSION_CODES.S)
    void startForegroundServiceS(Intent intent) {
        try {
            try {
                startForegroundService(intent);
            } catch (ForegroundServiceStartNotAllowedException e) {
                Log.w(TAG, "ForegroundServiceStartNotAllowedException: " + e);

                // The service is already running, so maybe try saving without trying to put it in
                // foreground. According to docs we should have a couple of minutes of runtime.
                startService(intent);
            }
        } catch (ServiceStartNotAllowedException e) {
            // We may not be allowed to start a background service either,
            // probably because onPause is called on an already-paused
            // application that is beyond the "couple of minutes" cutoff.
            Log.w(TAG, "ServiceStartNotAllowedException: " + e);
        }
    }

    @Override
    public void onDestroy() {
        // Docs say: this method will not be called if the activity's hosting process
        // is killed. This means, for us that the service has been stopped.

        Log.i(TAG, "[onDestroy]");
        if (mPenEngine != null) {
            mPenEngine.unbind();
            mPenEngine = null;
        }
        startServiceGeneric(DocumentSaverService.KILL_PROCESS);

        super.onDestroy();
    }

    @Override
    public boolean onKeyUp(final int keyCode, final KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            if (!JNIWrappers.hasMainWindowLoaded()) {
                // back button was pressed during splash screen, letting this
                // propagate leaves native side in an undefined state. So, it's
                // best we finish the activity here.
                finish();
            }
        }

        return super.onKeyUp(keyCode, event);
    }

    /**
     * Vendor stylus hardware (Xiaomi Focus Pen and similar) reports its gestures as
     * non-standard key codes. Qt never turns these into key events, so capture them
     * here and hand them to native, which maps them onto the remappable stylus
     * actions. Rebind those actions in the stylus settings to change what they do.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        final int keyCode = event.getKeyCode();
        if (keyCode >= STYLUS_GESTURE_KEY_FIRST && keyCode <= STYLUS_GESTURE_KEY_LAST) {
            JNIWrappers.stylusGestureKey(keyCode, event.getAction(), event.getRepeatCount());
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        // We manually pass these events to the QPA Android because,
        // android doesn't send events of type other than SOURCE_CLASS_POINTER
        // to the view which was just tapped. So, this view will never get to
        // QtSurface, because it doesn't claim focus.
        // The vendor posture stream carries the barrel rotation. Qt does not read the
        // axis this device populates, so we decode it ourselves and hand it to native.
        if (mPenEngine != null && mPenEngine.handleMotionEvent(event)) {
            return true;
        }

        if (event.isFromSource(InputDevice.SOURCE_TOUCHPAD)) {
            return QtNative.getInputEventDispatcher().sendGenericMotionEvent(event, event.getDeviceId());
        }
        return super.onGenericMotionEvent(event);
    }

    public void onUserInteraction() {
    }

    public void copyAssets() {
        new ConfigsManager(this).handleAssets();
    }

    public boolean isInFullScreen() {
        return inFullScreen;
    }

    public void setFullScreenOnUiThread(boolean fullScreen) {
        if (fullScreen != inFullScreen) {
            runOnUiThread(() -> trySetFullScreen(fullScreen));
        }
    }

    private void trySetFullScreen(boolean fullScreen) {
        try {
            setFullScreen(fullScreen);
            inFullScreen = fullScreen;
        } catch (Exception | UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to set fullscreen " + fullScreen, e);
        }
    }

    public static int getLongPressTimeout() {
        try {
            return ViewConfiguration.get(QtNative.activity()).getLongPressTimeout();
        } catch (Exception|UnsatisfiedLinkError e) {
            Log.e(TAG, "Exception getting long press timeout", e);
            return 500;
        }
    }

    public static boolean looksLikeXiaomiDevice() {
        return containsXiaomi(Build.BRAND) || containsXiaomi(Build.MANUFACTURER);
    }

    private static boolean containsXiaomi(String s) {
        return s != null && s.toLowerCase().contains("xiaomi");
    }

    public ApplicationExitInfo getLastApplicationExitInfo() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                ActivityManager activityManager = getSystemService(ActivityManager.class);
                if (activityManager != null) {
                    List<ApplicationExitInfo> exitReasons =
                        activityManager.getHistoricalProcessExitReasons(null, 0, 1);
                    if (exitReasons != null && !exitReasons.isEmpty()) {
                        return exitReasons.get(0);
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "Exception getting last application exit info", e);
            }
        }
        return null;
    }

    public static boolean isLowMemoryKillReportSupported() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                return ActivityManager.isLowMemoryKillReportSupported();
            } catch (Exception e) {
                Log.e(TAG, "Exception getting low memory kill report support", e);
            }
        }
        return false;
    }

    public void showScalingDialog(double currentScale, double defaultScale, boolean showOnStartup, boolean canShowOnStartup) {
        QtNative.activity().runOnUiThread(() -> {
            ScalingDialog scalingDialog = new ScalingDialog(this, currentScale, defaultScale, showOnStartup, canShowOnStartup);
            scalingDialog.show();
        });
    }
}
