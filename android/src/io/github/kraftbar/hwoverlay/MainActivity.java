package io.github.kraftbar.hwoverlay;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.graphics.Insets;
import android.os.Bundle;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import android.widget.FrameLayout;

/**
 * Shows the hw-overlay stats page from the PC. The page's "Keep screen on"
 * button calls into this activity (window.HwApp), which keeps the screen on
 * and hides the system bars - no Wake Lock or HTTPS needed.
 */
public class MainActivity extends Activity {
    private static final String DEFAULT_URL = "http://192.168.1.114:8787/";

    private SharedPreferences prefs;
    private WebView web;
    private boolean awake;
    private AlertDialog addressDialog;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("hw", MODE_PRIVATE);
        awake = prefs.getBoolean("awake", false);

        web = new WebView(this);
        web.setBackgroundColor(0xFF111111);
        web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setDomStorageEnabled(true);
        web.addJavascriptInterface(new Bridge(), "HwApp");
        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onReceivedError(WebView view, WebResourceRequest req, WebResourceError err) {
                if (req.isForMainFrame()) askAddress();
            }
        });
        // Android 15 draws edge to edge: keep the page clear of the status
        // and navigation bars while they are shown.
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF111111);
        root.addView(web);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return WindowInsets.CONSUMED;
        });
        setContentView(root);
        web.loadUrl(prefs.getString("url", DEFAULT_URL));
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyAwake();   // the system shows the bars again after leaving the app
    }

    private void applyAwake() {
        WindowInsetsController bars = getWindow().getInsetsController();
        if (awake) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            if (bars != null) {
                bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                bars.hide(WindowInsets.Type.systemBars());
            }
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            if (bars != null) bars.show(WindowInsets.Type.systemBars());
        }
    }

    /** Ask for the PC's address, e.g. when the page can't be reached. */
    private void askAddress() {
        if (addressDialog != null && addressDialog.isShowing()) return;
        final EditText input = new EditText(this);
        input.setSingleLine();
        input.setText(prefs.getString("url", DEFAULT_URL));
        addressDialog = new AlertDialog.Builder(this)
                .setTitle("PC address")
                .setMessage("The address shown in the overlay's tray menu.")
                .setView(input)
                .setPositiveButton("Connect", (d, w) -> {
                    String url = input.getText().toString().trim();
                    if (!url.contains("://")) url = "http://" + url;
                    prefs.edit().putString("url", url).apply();
                    web.loadUrl(url);
                })
                .setNegativeButton("Retry", (d, w) -> web.reload())
                .show();
    }

    @Override
    public void onBackPressed() {
        if (web.canGoBack()) web.goBack();
        else super.onBackPressed();
    }

    /** Called from the page's JavaScript (on a background thread). */
    private class Bridge {
        @JavascriptInterface
        public boolean isAwake() {
            return awake;
        }

        @JavascriptInterface
        public void setAwake(boolean on) {
            runOnUiThread(() -> {
                awake = on;
                prefs.edit().putBoolean("awake", on).apply();
                applyAwake();
            });
        }

        @JavascriptInterface
        public void changeAddress() {
            runOnUiThread(MainActivity.this::askAddress);
        }
    }
}
