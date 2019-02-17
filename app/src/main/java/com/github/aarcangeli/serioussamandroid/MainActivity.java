package com.github.aarcangeli.serioussamandroid;

import android.Manifest;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Environment;
import android.support.annotation.NonNull;
import android.support.v4.app.ActivityCompat;
import android.support.v4.content.ContextCompat;
import android.support.v7.app.AppCompatActivity;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

public class MainActivity extends AppCompatActivity {
    public static final String TAG = "SeriousSamJava";
    private final int REQUEST_WRITE_STORAGE = 1;

    private static final int AXIS_MOVE_UD = 0;
    private static final int AXIS_MOVE_LR = 1;
    private static final int AXIS_MOVE_FB = 2;
    private static final int AXIS_TURN_UD = 3;
    private static final int AXIS_TURN_LR = 4;
    private static final int AXIS_TURN_BK = 5;
    private static final int AXIS_LOOK_UD = 6;
    private static final int AXIS_LOOK_LR = 7;
    private static final int AXIS_LOOK_BK = 8;

    private static final float VIEW_MULT = 2;

    private SeriousSamSurface glSurfaceView;
    private File homeDir;

    private AtomicBoolean printProfiling = new AtomicBoolean();

    private boolean isGameStarted = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setContentView(R.layout.main_screen);
        glSurfaceView = findViewById(R.id.main_content);

        homeDir = new File(Environment.getExternalStorageDirectory(), "SeriousSam");
        Log.i(TAG, "HomeDir: " + homeDir.getAbsolutePath());
        nSetHomeDir(homeDir.getAbsolutePath());

        checkPermission();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // hide verything from the screen
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION);

        if (isGameStarted) {
            glSurfaceView.start();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        glSurfaceView.stop();
    }

    private void checkPermission() {
        int permission = ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE);
        if (permission != PackageManager.PERMISSION_GRANTED) {
            requestPermission();
        } else {
            startGame();
        }
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        if (ev.getAction() == MotionEvent.ACTION_MOVE) {
            setAxisValue(AXIS_MOVE_FB, -ev.getAxisValue(MotionEvent.AXIS_Y));
            setAxisValue(AXIS_MOVE_LR, -ev.getAxisValue(MotionEvent.AXIS_X));
            setAxisValue(AXIS_LOOK_LR, -ev.getAxisValue(MotionEvent.AXIS_Z) * VIEW_MULT);
            setAxisValue(AXIS_LOOK_UD, -ev.getAxisValue(MotionEvent.AXIS_RZ) * VIEW_MULT);
            nDispatchKeyEvent(KeyEvent.KEYCODE_BUTTON_R2, ev.getAxisValue(MotionEvent.AXIS_RTRIGGER) > .5f ? 1 : 0);
            nDispatchKeyEvent(KeyEvent.KEYCODE_BUTTON_L2, ev.getAxisValue(MotionEvent.AXIS_RTRIGGER) < -.5f ? 1 : 0);
            nDispatchKeyEvent(KeyEvent.KEYCODE_DPAD_LEFT, ev.getAxisValue(MotionEvent.AXIS_HAT_X) < -.5f ? 1 : 0);
            nDispatchKeyEvent(KeyEvent.KEYCODE_DPAD_RIGHT, ev.getAxisValue(MotionEvent.AXIS_HAT_X) > .5f ? 1 : 0);
            nDispatchKeyEvent(KeyEvent.KEYCODE_DPAD_UP, ev.getAxisValue(MotionEvent.AXIS_HAT_Y) < -.5f ? 1 : 0);
            nDispatchKeyEvent(KeyEvent.KEYCODE_DPAD_DOWN, ev.getAxisValue(MotionEvent.AXIS_HAT_Y) > .5f ? 1 : 0);
        }
        return true;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getRepeatCount() == 0) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                nDispatchKeyEvent(event.getKeyCode(), 1);
                System.out.println(event.getKeyCode());
            }
            if (event.getAction() == KeyEvent.ACTION_UP) {
                nDispatchKeyEvent(event.getKeyCode(), 0);
            }
        }
        return true;
    }

    private void requestPermission() {
        ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_WRITE_STORAGE);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        if (requestCode == REQUEST_WRITE_STORAGE) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                startGame();
            } else {
                requestPermission();
            }
        }
    }

    // ui listeners
    public void hideMenu(View view) {
    }

    public void doProfiling(View view) {
        printProfilingData();
    }

    private void startGame() {
        if (!homeDir.exists()) homeDir.mkdirs();
        isGameStarted = true;
        glSurfaceView.start();
    }

    private static native void setAxisValue(int key, float value);
    private static native void printProfilingData();
    private static native void nDispatchKeyEvent(int key, int isPressed);
    private static native void nSetHomeDir(String homeDir);
}
