package com.kneepad.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.le.ScanResult;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.ArrayAdapter;
import android.widget.TextView;
import android.widget.Toast;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

public class MainActivity extends Activity {
    private static final int BLUE = Color.rgb(40, 103, 232);
    private static final int INK = Color.rgb(23, 32, 51);
    private static final int MUTED = Color.rgb(107, 118, 144);
    private static final int BG = Color.rgb(244, 247, 251);
    private static final int RED = Color.rgb(220, 60, 77);
    private static final int GREEN = Color.rgb(22, 136, 91);
    private static final int REQ_PICK_AVATAR = 300;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private DeviceClient api;
    private AiClient aiClient;
    private TrainingDatabase database;
    private AuthManager auth;
    private FrameLayout content;
    private LinearLayout navigation;
    private TextView title;
    private TextView connectionText;
    private Button liveTab;
    private Button historyTab;
    private Button profileTab;
    private int currentTab;
    private boolean resumed;
    private boolean polling;
    private boolean profileDialogVisible;
    private boolean profileFormOpening;
    private boolean pairDialogVisible;
    private AlertDialog activeProfileDialog;
    private AlertDialog activeProfileFormDialog;
    private ProfileState profileState;
    private LiveData latest;
    private SessionAccumulator session;

    private TextView userText;
    private TextView sessionText;
    private TextView scoreText;
    private TextView statusText;
    private TextView actionText;
    private TextView countText;
    private TextView qualityText;
    private TextView contactText;
    private TextView kneeText;
    private TextView kneeDetailText;
    private TextView kneeMapText;
    private TextView adviceText;
    private TextView deviceText;
    private ProgressBar targetProgress;
    private PressureGaugeView pressureGauge;
    private RepsRingView repsRing;
    private FatigueChartView fatigueChart;
    private AvatarView profileAvatar;
    private Button trainingButton;
    private String retainedQualityText = "等待完成动作";
    private String retainedContactText = "等待运动数据";
    private int retainedContactColor = MUTED;
    private String lastDisplayedAction = "UNKNOWN";
    private boolean outwardEpisodeActive;
    private boolean outwardEpisodeShowsNormal;
    private int currentWalk;
    private int currentSquat;
    private int currentDeadlift;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        api = new DeviceClient(this, new BleClient.Listener() {
            @Override public void onLiveJson(String json) {
            }

            @Override public void onState(String message) {
                setConnected(true, message);
            }

            @Override public void onError(String message) {
                setConnected(false, message);
            }
        });
        aiClient = new AiClient(this);
        database = new TrainingDatabase(this);
        auth = new AuthManager(this);
        buildShell();
        if (auth.isLoggedIn()) {
            showLivePage();
            beginDeviceConnection();
        } else {
            showLoginPage();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        if (!auth.isLoggedIn()) return;
        beginDeviceConnection();
    }

    @Override
    protected void onPause() {
        super.onPause();
        resumed = false;
        handler.removeCallbacksAndMessages(null);
    }

    @Override
    protected void onDestroy() {
        api.shutdown();
        aiClient.shutdown();
        database.close();
        super.onDestroy();
    }

    private void buildShell() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);

        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(18), dp(12), dp(14), dp(12));
        top.setBackgroundColor(BLUE);
        top.setElevation(dp(4));
        title = text("智能护膝", 21, Color.WHITE, true);
        top.addView(title, new LinearLayout.LayoutParams(0, dp(52), 1));
        connectionText = text("正在检查设备", 12, Color.WHITE, false);
        connectionText.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        top.addView(connectionText, new LinearLayout.LayoutParams(dp(145), dp(52)));
        root.addView(top);

        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        LinearLayout navigation = new LinearLayout(this);
        this.navigation = navigation;
        navigation.setBackgroundColor(Color.WHITE);
        navigation.setPadding(dp(8), dp(5), dp(8), dp(5));
        navigation.setElevation(dp(6));
        liveTab = navButton("实时监测", () -> showLivePage());
        historyTab = navButton("训练历史", () -> showHistoryPage());
        profileTab = navButton("我的档案", () -> showProfilePage());
        navigation.addView(liveTab, weighted());
        navigation.addView(historyTab, weighted());
        navigation.addView(profileTab, weighted());
        root.addView(navigation, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(60)));
        setContentView(root);
    }

    private void showLivePage() {
        currentTab = 0;
        selectTab(liveTab);
        if (navigation != null) navigation.setVisibility(View.VISIBLE);
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = vertical(dp(14));
        page.setPadding(dp(14), dp(14), dp(14), dp(24));

        LinearLayout deviceCard = card();
        TextView deviceTitle = text("护膝连接", 14, INK, true);
        deviceCard.addView(deviceTitle);
        deviceText = text("请将手机连接到 KneePad_ESP32 热点", 13, MUTED, false);
        deviceText.setPadding(0, dp(7), 0, dp(8));
        deviceCard.addView(deviceText);
        LinearLayout deviceActions = horizontal();
        Button wifi = primaryButton("打开 Wi-Fi 设置");
        wifi.setOnClickListener(v -> openWifiSettings());
        Button retry = secondaryButton("重新检查");
        retry.setOnClickListener(v -> { beginDeviceConnection(); schedulePoll(0); });
        deviceActions.addView(wifi, weighted());
        deviceActions.addView(space(dp(8)));
        deviceActions.addView(retry, weighted());
        deviceCard.addView(deviceActions);
        LinearLayout bleRow = horizontal();
        Button ble = secondaryButton(api.bleMode() ? "断开蓝牙（切回 Wi-Fi）" : "蓝牙连接（BLE）");
        ble.setOnClickListener(v -> toggleBleConnection());
        bleRow.addView(ble, weighted());
        deviceCard.addView(bleRow);
        page.addView(deviceCard);

        LinearLayout hero = card();
        hero.setBackgroundColor(BLUE);
        userText = text("你好，训练者", 22, Color.WHITE, true);
        hero.addView(userText);
        sessionText = text("连接设备并选择个人档案", 13, Color.rgb(220, 231, 255), false);
        sessionText.setPadding(0, dp(8), 0, dp(8));
        hero.addView(sessionText);
        targetProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        targetProgress.setMax(100);
        hero.addView(targetProgress, match(dp(10)));
        page.addView(hero);

        LinearLayout row1 = horizontal();
        scoreText = metric(row1, "疲劳评分");
        statusText = metric(row1, "个性化状态");
        page.addView(row1);
        LinearLayout row2 = horizontal();
        actionText = metric(row2, "当前动作");
        countText = metric(row2, "本次动作");
        page.addView(row2);

        LinearLayout repsCard = card();
        repsCard.addView(text("动作分布（走路 / 深蹲 / 硬拉）", 13, MUTED, false));
        repsRing = new RepsRingView(this);
        repsCard.addView(repsRing, match(dp(160)));
        repsCard.addView(buildRepsLegend());
        page.addView(repsCard);

        LinearLayout qualityCard = card();
        qualityCard.addView(text("动作质量", 13, MUTED, false));
        qualityText = text(retainedQualityText, 20, INK, true);
        qualityText.setPadding(0, dp(7), 0, 0);
        qualityCard.addView(qualityText);
        page.addView(qualityCard);

        LinearLayout contactCard = card();
        contactCard.addView(text("肌电电极状态", 13, MUTED, false));
        contactText = text(retainedContactText, 20, retainedContactColor, true);
        contactText.setPadding(0, dp(7), 0, 0);
        contactCard.addView(contactText);
        page.addView(contactCard);

        LinearLayout kneeCard = card();
        kneeCard.addView(text("膝盖姿态（内扣/外翻）", 13, MUTED, false));
        kneeText = text("等待姿态数据", 20, INK, true);
        kneeText.setPadding(0, dp(7), 0, 0);
        kneeCard.addView(kneeText);
        kneeDetailText = text("", 13, MUTED, false);
        kneeDetailText.setPadding(0, dp(4), 0, 0);
        kneeCard.addView(kneeDetailText);
        kneeMapText = text(kneeMapLabel(), 12, BLUE, false);
        kneeMapText.setPadding(0, dp(6), 0, 0);
        kneeMapText.setOnClickListener(v -> toggleKneeMap());
        kneeCard.addView(kneeMapText);
        pressureGauge = new PressureGaugeView(this);
        pressureGauge.setMinimumHeight(dp(96));
        kneeCard.addView(pressureGauge, match(dp(104)));
        page.addView(kneeCard);

        LinearLayout fatigueCard = card();
        fatigueCard.addView(text("疲劳趋势", 13, MUTED, false));
        fatigueChart = new FatigueChartView(this);
        fatigueChart.setMinimumHeight(dp(150));
        fatigueCard.addView(fatigueChart, match(dp(168)));
        page.addView(fatigueCard);

        LinearLayout adviceCard = card();
        adviceCard.addView(text("个性化建议", 13, MUTED, false));
        adviceText = text("请先连接护膝并选择个人档案", 15, INK, false);
        adviceText.setPadding(0, dp(8), 0, 0);
        adviceCard.addView(adviceText);
        page.addView(adviceCard);

        trainingButton = primaryButton(session == null ? "开始训练" : "结束并生成报告");
        trainingButton.setOnClickListener(v -> toggleTraining());
        page.addView(trainingButton, match(dp(52)));
        scroll.addView(page);
        replaceContent(scroll);
        if (latest != null) renderLive(latest);
    }

    private void showHistoryPage() {
        currentTab = 1;
        selectTab(historyTab);
        if (navigation != null) navigation.setVisibility(View.VISIBLE);
        List<TrainingSession> sessions = database.allSessions();
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = vertical(dp(12));
        page.setPadding(dp(14), dp(14), dp(14), dp(24));
        LinearLayout summary = card();
        int totalReps = 0;
        long totalSeconds = 0;
        for (TrainingSession item : sessions) {
            totalReps += item.reps;
            totalSeconds += item.durationSeconds();
        }
        summary.addView(text("训练概览", 18, INK, true));
        summary.addView(text("共 " + sessions.size() + " 次训练 · " + totalReps + " 个动作 · " + formatDuration(totalSeconds), 14, MUTED, false));
        page.addView(summary);

        LinearLayout exportCard = card();
        exportCard.addView(text("数据导出", 16, INK, true));
        Button exportCsv = primaryButton("导出全部训练记录 CSV");
        exportCsv.setOnClickListener(v -> exportCsv());
        exportCard.addView(exportCsv, match(dp(48)));
        page.addView(exportCard);

        AiClient.Settings aiSettings = aiClient.settings();
        LinearLayout aiCard = card();
        aiCard.addView(text("AI运动分析", 18, INK, true));
        aiCard.addView(text(aiSettings.isConfigured()
                ? "已配置模型：" + aiSettings.model + "。训练结束后可在报告中生成AI建议。"
                : "尚未配置AI服务。请先填写接口地址、模型名称，并按接口要求填写 API Key。", 13, MUTED, false));
        Button generateAiButton = primaryButton("生成最新 AI 报告");
        generateAiButton.setOnClickListener(v -> {
            if (sessions.isEmpty()) {
                new AlertDialog.Builder(this)
                        .setTitle("暂无训练数据")
                        .setMessage("请先在“实时监测”页完成一次训练。训练结束并保存后，即可在这里生成最新 AI 报告。")
                        .setPositiveButton("去实时监测", (dialog, which) -> showLivePage())
                        .setNegativeButton("取消", null)
                        .show();
            } else {
                requestAiAdvice(sessions.get(0));
            }
        });
        aiCard.addView(generateAiButton, match(dp(48)));
        Button aiSettingsButton = secondaryButton(aiSettings.isConfigured() ? "修改 AI 设置" : "AI 服务设置");
        aiSettingsButton.setOnClickListener(v -> showAiSettingsDialog(null));
        aiCard.addView(aiSettingsButton, match(dp(48)));
        page.addView(aiCard);

        TrainingChartView chart = new TrainingChartView(this);
        chart.setSessions(sessions);
        page.addView(chart, match(dp(220)));
        if (sessions.isEmpty()) {
            LinearLayout empty = card();
            empty.addView(text("暂无训练记录", 17, INK, true));
            empty.addView(text("连接护膝，在实时监测页点击“开始训练”，结束后会自动生成报告。", 14, MUTED, false));
            page.addView(empty);
        } else {
            for (TrainingSession item : sessions) page.addView(historyCard(item));
        }
        scroll.addView(page);
        replaceContent(scroll);
    }

    private void showProfilePage() {
        currentTab = 2;
        selectTab(profileTab);
        if (navigation != null) navigation.setVisibility(View.VISIBLE);
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = vertical(dp(12));
        page.setPadding(dp(14), dp(14), dp(14), dp(24));
        LinearLayout intro = card();
        LinearLayout header = horizontal();
        header.setGravity(Gravity.CENTER_VERTICAL);
        profileAvatar = new AvatarView(this);
        LinearLayout.LayoutParams avatarParams = new LinearLayout.LayoutParams(dp(76), dp(76));
        header.addView(profileAvatar, avatarParams);
        LinearLayout headerText = vertical(dp(4));
        headerText.setPadding(dp(16), 0, 0, 0);
        headerText.addView(text(auth.currentUser(), 20, INK, true));
        Button changeAvatar = secondaryButton("更换头像");
        changeAvatar.setPadding(dp(10), 0, dp(10), 0);
        changeAvatar.setOnClickListener(v -> pickAvatar());
        headerText.addView(changeAvatar, new LinearLayout.LayoutParams(dp(150), dp(40)));
        header.addView(headerText, weighted());
        renderAvatar();
        intro.addView(header);
        intro.addView(text("个人档案", 17, INK, true));
        intro.addView(text("档案保存在护膝 ESP32 中，训练历史保存在本手机中。", 13, MUTED, false));
        Button refresh = primaryButton("读取护膝档案");
        refresh.setOnClickListener(v -> checkProfiles(true));
        intro.addView(refresh, match(dp(48)));
        page.addView(intro);
        if (profileState == null) {
            page.addView(cardText("尚未读取档案，请先连接 KneePad_ESP32 热点。"));
        } else {
            for (UserProfile profile : profileState.profiles) {
                boolean isCurrent = profileState.active && profileState.profile != null && profile.id == profileState.profile.id;
                LinearLayout item = card();
                item.addView(text(profile.name + (isCurrent ? "（当前）" : ""), 17, INK, true));
                item.addView(text(profile.age + " 岁 · " + profile.habit + " · " + profile.goal + "\nBMI " + String.format(Locale.CHINA, "%.1f", profile.bmi) + " · 累计 " + profile.totalReps + " 次", 13, MUTED, false));
                LinearLayout actions = horizontal();
                Button use = primaryButton(isCurrent ? "当前使用者" : "切换使用者");
                use.setEnabled(!isCurrent);
                use.setOnClickListener(v -> selectProfile(profile.id));
                Button edit = secondaryButton("编辑");
                edit.setOnClickListener(v -> showProfileForm(profile));
                Button delete = secondaryButton("删除");
                delete.setTextColor(RED);
                delete.setOnClickListener(v -> confirmDeleteProfile(profile));
                actions.addView(use, new LinearLayout.LayoutParams(0, dp(48), 1.4f));
                actions.addView(space(dp(6)));
                actions.addView(edit, new LinearLayout.LayoutParams(0, dp(48), 0.8f));
                actions.addView(space(dp(6)));
                actions.addView(delete, new LinearLayout.LayoutParams(0, dp(48), 0.8f));
                item.addView(actions);
                page.addView(item);
            }
            Button create = primaryButton("新建个人档案");
            create.setOnClickListener(v -> showProfileForm(null));
            page.addView(create, match(dp(52)));
        }
        LinearLayout help = card();
        help.addView(text("连接方法", 17, INK, true));
        help.addView(text("1. 打开智能护膝\n2. 手机连接 KneePad_ESP32\n3. 热点密码 12345678\n4. 返回本 App 自动连接设备\n\n手机提示“该网络无法访问互联网”时，请选择仍然连接。", 14, MUTED, false));
        Button logout = secondaryButton("退出登录（" + auth.currentUser() + "）");
        logout.setTextColor(RED);
        logout.setOnClickListener(v -> confirmLogout());
        help.addView(logout, match(dp(48)));
        page.addView(help);
        scroll.addView(page);
        replaceContent(scroll);
    }

    private void confirmLogout() {
        new AlertDialog.Builder(this)
                .setTitle("退出登录")
                .setMessage("确定退出当前账号吗？训练记录仍会保留在本机。")
                .setPositiveButton("退出", (dialog, which) -> {
                    auth.logout();
                    api.ble().disconnect();
                    api.setBleMode(false);
                    showLoginPage();
                })
                .setNegativeButton("取消", null)
                .show();
    }

    // ===================== 头像 =====================

    private java.io.File avatarFile() {
        java.io.File dir = new java.io.File(getFilesDir(), "avatars");
        return new java.io.File(dir, auth.currentUser() + ".jpg");
    }

    private Bitmap loadAvatar() {
        try {
            java.io.File file = avatarFile();
            if (file.exists()) return BitmapFactory.decodeFile(file.getAbsolutePath());
        } catch (Exception ignored) {
        }
        return null;
    }

    private void renderAvatar() {
        if (profileAvatar == null) return;
        Bitmap bmp = loadAvatar();
        if (bmp != null) {
            profileAvatar.setAvatar(bmp);
        } else {
            String name = auth.currentUser();
            String first = name.isEmpty() ? "护" : name.substring(0, 1).toUpperCase(java.util.Locale.CHINA);
            profileAvatar.setLabel(first, avatarColor(name));
        }
    }

    private static int avatarColor(String name) {
        int hue = Math.abs(name == null ? 0 : name.hashCode()) % 360;
        return Color.HSVToColor(new float[]{hue, 0.55f, 0.85f});
    }

    private void pickAvatar() {
        Intent intent;
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            intent = new Intent(android.provider.MediaStore.ACTION_PICK_IMAGES);
        } else {
            intent = new Intent(Intent.ACTION_GET_CONTENT);
            intent.setType("image/*");
        }
        try {
            startActivityForResult(intent, REQ_PICK_AVATAR);
        } catch (Exception exception) {
            Intent fallback = new Intent(Intent.ACTION_GET_CONTENT);
            fallback.setType("image/*");
            try {
                startActivityForResult(fallback, REQ_PICK_AVATAR);
            } catch (Exception exception2) {
                toast("无法打开相册");
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_AVATAR && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) saveAvatarFromUri(uri);
        }
    }

    private void saveAvatarFromUri(Uri uri) {
        try {
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inJustDecodeBounds = true;
            java.io.InputStream in = getContentResolver().openInputStream(uri);
            BitmapFactory.decodeStream(in, null, options);
            if (in != null) in.close();

            int size = 256;
            int sample = 1;
            while ((options.outWidth / sample) > size * 2 || (options.outHeight / sample) > size * 2) {
                sample *= 2;
            }
            options.inJustDecodeBounds = false;
            options.inSampleSize = sample;
            in = getContentResolver().openInputStream(uri);
            Bitmap bitmap = BitmapFactory.decodeStream(in, null, options);
            if (in != null) in.close();
            if (bitmap == null) {
                toast("无法读取图片");
                return;
            }
            Bitmap square = centerCrop(bitmap, size);
            if (square != bitmap) bitmap.recycle();

            java.io.File file = avatarFile();
            java.io.File dir = file.getParentFile();
            if (dir != null && !dir.exists() && !dir.mkdirs()) {
                toast("无法创建头像目录");
                return;
            }
            java.io.FileOutputStream out = new java.io.FileOutputStream(file);
            square.compress(Bitmap.CompressFormat.JPEG, 92, out);
            out.close();
            renderAvatar();
            toast("头像已更新");
        } catch (Exception exception) {
            toast("头像保存失败");
        }
    }

    private static Bitmap centerCrop(Bitmap src, int size) {
        int w = src.getWidth();
        int h = src.getHeight();
        int s = Math.min(w, h);
        int x = (w - s) / 2;
        int y = (h - s) / 2;
        Bitmap crop = Bitmap.createBitmap(src, x, y, s, s);
        return Bitmap.createScaledBitmap(crop, size, size, true);
    }

    // ===================== 分项环图图例（与环图为兄弟元素，杜绝重叠） =====================

        private LinearLayout repsLegendRow;
    private TextView repsLegendWalk;
    private TextView repsLegendSquat;
    private TextView repsLegendDeadlift;

    private LinearLayout buildRepsLegend() {
        LinearLayout row = horizontal();
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(4), 0, dp(4));
        repsLegendWalk = addRepsLegendItem(row, RepsRingView.WALK_COLOR, "走路", currentWalk);
        repsLegendSquat = addRepsLegendItem(row, RepsRingView.SQUAT_COLOR, "深蹲", currentSquat);
        repsLegendDeadlift = addRepsLegendItem(row, RepsRingView.DEADLIFT_COLOR, "硬拉", currentDeadlift);
        return row;
    }

    private TextView addRepsLegendItem(LinearLayout row, int color, String name, int value) {
        LinearLayout item = vertical(dp(2));
        item.setGravity(Gravity.CENTER);
        android.graphics.drawable.GradientDrawable dot = new android.graphics.drawable.GradientDrawable();
        dot.setColor(color);
        dot.setShape(android.graphics.drawable.GradientDrawable.OVAL);
        View dotView = new View(this);
        dotView.setBackground(dot);
        LinearLayout.LayoutParams dotParams = new LinearLayout.LayoutParams(dp(10), dp(10));
        item.addView(dotView, dotParams);
        TextView label = text(name + " " + value, 11, INK, false);
        item.addView(label);
        LinearLayout.LayoutParams itemParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        itemParams.setMargins(dp(8), 0, dp(8), 0);
        row.addView(item, itemParams);
        return label;
    }

    // ===================== 账号登录 / 注册 =====================

    private void showLoginPage() {
        currentTab = -1;
        selectTab(null);
        if (navigation != null) navigation.setVisibility(View.GONE);
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = vertical(dp(14));
        page.setPadding(dp(28), dp(64), dp(28), dp(24));

        LinearLayout logoRow = horizontal();
        logoRow.setGravity(Gravity.CENTER);
        AvatarView logo = new AvatarView(this);
        logo.setLabel("护", BLUE);
        logoRow.addView(logo, new LinearLayout.LayoutParams(dp(88), dp(88)));
        page.addView(logoRow);

        TextView title = text("智能护膝", 32, INK, true);
        title.setGravity(Gravity.CENTER);
        page.addView(title);
        TextView subtitle = text("运动监测 · 康复训练助手", 14, MUTED, false);
        subtitle.setGravity(Gravity.CENTER);
        subtitle.setPadding(0, dp(6), 0, dp(30));
        page.addView(subtitle);

        EditText username = input("用户名", InputType.TYPE_CLASS_TEXT, "");
        username.setSingleLine(true);
        page.addView(username);

        EditText password = input("密码", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD, "");
        password.setSingleLine(true);
        page.addView(password);

        Button loginBtn = primaryButton("登 录");
        loginBtn.setOnClickListener(v -> {
            String err = auth.login(username.getText().toString(), password.getText().toString());
            if (err != null) {
                toast(err);
            } else {
                toast("登录成功，欢迎 " + auth.currentUser());
                showLivePage();
                beginDeviceConnection();
            }
        });
        page.addView(loginBtn, match(dp(52)));

        Button registerBtn = secondaryButton("没有账号？立即注册");
        registerBtn.setOnClickListener(v -> showRegisterPage());
        page.addView(registerBtn, match(dp(50)));

        scroll.addView(page);
        replaceContent(scroll);
    }

    private void showRegisterPage() {
        currentTab = -1;
        selectTab(null);
        if (navigation != null) navigation.setVisibility(View.GONE);
        ScrollView scroll = new ScrollView(this);
        LinearLayout page = vertical(dp(14));
        page.setPadding(dp(28), dp(56), dp(28), dp(24));

        TextView title = text("注册账号", 28, INK, true);
        title.setGravity(Gravity.CENTER);
        page.addView(title);
        TextView subtitle = text("创建账号，开始科学训练", 14, MUTED, false);
        subtitle.setGravity(Gravity.CENTER);
        subtitle.setPadding(0, dp(6), 0, dp(26));
        page.addView(subtitle);

        EditText username = input("用户名（2-20 字符）", InputType.TYPE_CLASS_TEXT, "");
        username.setSingleLine(true);
        page.addView(username);

        EditText password = input("密码（至少 4 位）", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD, "");
        password.setSingleLine(true);
        page.addView(password);

        EditText confirm = input("确认密码", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD, "");
        confirm.setSingleLine(true);
        page.addView(confirm);

        Button registerBtn = primaryButton("注 册");
        registerBtn.setOnClickListener(v -> {
            String user = username.getText().toString().trim();
            String pwd = password.getText().toString();
            String pwd2 = confirm.getText().toString();
            if (!pwd.equals(pwd2)) {
                toast("两次输入的密码不一致");
                return;
            }
            String err = auth.register(user, pwd);
            if (err != null) {
                toast(err);
            } else {
                toast("注册成功，欢迎 " + auth.currentUser());
                showLivePage();
                beginDeviceConnection();
            }
        });
        page.addView(registerBtn, match(dp(52)));

        Button backBtn = secondaryButton("返回登录");
        backBtn.setOnClickListener(v -> showLoginPage());
        page.addView(backBtn, match(dp(50)));

        scroll.addView(page);
        replaceContent(scroll);
    }

    private void beginDeviceConnection() {
        verifyStoredPairCode();
    }

    // ===================== 蓝牙连接 =====================

    private void toggleBleConnection() {
        if (api.bleMode()) {
            api.ble().disconnect();
            api.setBleMode(false);
            setConnected(false, "已切换回 Wi-Fi 模式");
            showLivePage();
            return;
        }
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            if (checkSelfPermission(android.Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED ||
                    checkSelfPermission(android.Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{
                        android.Manifest.permission.BLUETOOTH_SCAN,
                        android.Manifest.permission.BLUETOOTH_CONNECT}, 200);
                return;
            }
        } else if (checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{android.Manifest.permission.ACCESS_FINE_LOCATION}, 201);
            return;
        }
        showBleScanDialog();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        boolean granted = grantResults.length > 0;
        for (int result : grantResults) granted = granted && result == PackageManager.PERMISSION_GRANTED;
        if (granted) {
            // 授权后不立即扫描，避免在权限回调栈里做复杂操作导致崩溃；
            // 提示用户再次点击“蓝牙连接”即可开始扫描。
            toast("权限已授予，请再次点击“蓝牙连接”开始扫描");
        } else {
            toast("未获得蓝牙权限，无法连接护膝");
        }
    }

    private void showBleScanDialog() {
        if (!api.ble().hasAdapter()) {
            new AlertDialog.Builder(this)
                    .setTitle("蓝牙连接")
                    .setMessage("未检测到蓝牙，请先开启手机蓝牙。")
                    .setPositiveButton("开启蓝牙", (d, w) -> api.ble().enableBluetooth())
                    .setNegativeButton("取消", null)
                    .show();
            return;
        }
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("扫描护膝设备")
                .setMessage("正在扫描 KneePad 设备…\n（护膝 ESP32 需已烧录 BLE 透传固件，否则扫描不到）")
                .setNegativeButton("取消", (d, w) -> api.ble().stopScan())
                .create();
        dialog.show();
        api.ble().scan(new ApiClient.Callback<List<ScanResult>>() {
            @Override public void onSuccess(List<ScanResult> results) {
                if (dialog.isShowing()) dialog.dismiss();
                if (isFinishing()) return;
                showBleDevicePicker(results);
            }

            @Override public void onError(String message) {
                if (dialog.isShowing()) dialog.dismiss();
                toast(message);
            }
        });
    }

    private void showBleDevicePicker(List<ScanResult> results) {
        LinearLayout body = vertical(dp(6));
        body.setPadding(dp(8), dp(6), dp(8), dp(6));
        for (ScanResult result : results) {
            String name = result.getDevice().getName();
            if (name == null) name = "未知设备";
            Button button = secondaryButton(name + "  ·  " + result.getDevice().getAddress());
            button.setOnClickListener(v -> connectBle(result.getDevice()));
            body.addView(button, match(dp(52)));
        }
        new AlertDialog.Builder(this)
                .setTitle("选择护膝设备")
                .setView(body)
                .setNegativeButton("取消", null)
                .show();
    }

    private void connectBle(BluetoothDevice device) {
        final String label = device.getName() == null ? device.getAddress() : device.getName();
        AlertDialog loading = new AlertDialog.Builder(this)
                .setTitle("蓝牙连接")
                .setMessage("正在连接 " + label + " …")
                .setNegativeButton("取消", (d, w) -> api.ble().disconnect())
                .create();
        loading.show();
        api.ble().connect(device, new ApiClient.Callback<Boolean>() {
            @Override public void onSuccess(Boolean value) {
                if (loading.isShowing()) loading.dismiss();
                api.setBleMode(true);
                setConnected(true, "蓝牙已连接 · 正在读取档案");
                checkProfiles(false);
                schedulePoll(0);
            }

            @Override public void onError(String message) {
                if (loading.isShowing()) loading.dismiss();
                toast(message);
            }
        });
    }

    // ===================== 训练记录导出 CSV =====================

    private void exportCsv() {
        List<TrainingSession> sessions = database.allSessions();
        if (sessions.isEmpty()) {
            toast("暂无训练记录可导出");
            return;
        }
        try {
            java.io.File dir = new java.io.File(getCacheDir(), "exports");
            if (!dir.exists() && !dir.mkdirs()) {
                toast("无法创建导出目录");
                return;
            }
            java.io.File file = new java.io.File(dir, "smart_knee_training_" +
                    new SimpleDateFormat("yyyyMMdd_HHmm", Locale.CHINA).format(new Date()) + ".csv");
            StringBuilder csv = new StringBuilder();
            csv.append("序号,训练者,开始时间,结束时间,时长(秒),动作数,平均疲劳,峰值疲劳,平均质量,良好,待改进,错误,深蹲,硬拉,疲劳曲线\n");
            int index = 1;
            for (TrainingSession item : sessions) {
                csv.append(index++).append(',')
                        .append(csvEscape(item.userName)).append(',')
                        .append(formatCsvDate(item.startedAt)).append(',')
                        .append(formatCsvDate(item.endedAt)).append(',')
                        .append(item.durationSeconds()).append(',')
                        .append(item.reps).append(',')
                        .append(round(item.avgFatigue)).append(',')
                        .append(item.peakFatigue).append(',')
                        .append(round(item.avgQuality)).append(',')
                        .append(item.goodCount).append(',')
                        .append(item.improveCount).append(',')
                        .append(item.wrongCount).append(',')
                        .append(item.squatCount).append(',')
                        .append(item.deadliftCount).append(',')
                        .append('"').append(item.fatigueCurve == null ? "" : item.fatigueCurve.replace("\"", "\"\"")).append('"')
                        .append('\n');
            }
            java.io.FileOutputStream out = new java.io.FileOutputStream(file);
            out.write(0xEF);
            out.write(0xBB);
            out.write(0xBF);
            out.write(csv.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            out.close();
            Uri uri = CsvFileProvider.uriFor(file);
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("text/csv");
            share.putExtra(Intent.EXTRA_STREAM, uri);
            share.putExtra(Intent.EXTRA_SUBJECT, "智能护膝训练记录");
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, "分享训练记录 CSV"));
        } catch (Exception exception) {
            toast("导出失败：" + exception.getMessage());
        }
    }

    private static String csvEscape(String value) {
        if (value == null) return "";
        String escaped = value.replace("\"", "\"\"");
        return (escaped.contains(",") || escaped.contains("\"") || escaped.contains("\n"))
                ? "\"" + escaped + "\"" : escaped;
    }

    private static String formatCsvDate(long value) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.CHINA).format(new Date(value));
    }

    private void verifyStoredPairCode() {
        api.verifyPairCode(api.pairCode(), new ApiClient.Callback<Boolean>() {
            @Override public void onSuccess(Boolean value) {
                checkProfiles(false);
                schedulePoll(0);
            }
            @Override public void onError(String message) {
                setConnected(false, "未连接护膝，请连接 KneePad_ESP32 热点");
                schedulePoll(1500);
            }
        });
    }

    private void checkProfiles(boolean showMessage) {
        api.loadProfiles(new ApiClient.Callback<ProfileState>() {
            @Override public void onSuccess(ProfileState state) {
                profileState = state;
                setConnected(true, "设备已连接");
                if (currentTab == 2) showProfilePage();
                if (!state.active && !profileDialogVisible) showProfileChooser(true);
                else if (state.profile != null && currentTab == 0 && userText != null) userText.setText("你好，" + state.profile.name);
                if (showMessage) toast("已读取护膝档案");
            }
            @Override public void onError(String message) {
                setConnected(false, "未连接护膝");
                if (showMessage) showConnectionHelp(message);
            }
        });
    }

    private void showProfileChooser(boolean required) {
        if (profileState == null || profileDialogVisible || profileFormOpening ||
                (activeProfileDialog != null && activeProfileDialog.isShowing()) ||
                (activeProfileFormDialog != null && activeProfileFormDialog.isShowing())) return;
        profileDialogVisible = true;
        LinearLayout body = vertical(dp(8));
        body.setPadding(dp(4), 0, dp(4), 0);
        if (profileState.profiles.isEmpty()) {
            body.addView(text("还没有个人档案，请先新建。", 14, MUTED, false));
        } else {
            for (UserProfile profile : profileState.profiles) {
                Button item = secondaryButton(profile.name + " · " + profile.age + " 岁 · " + profile.habit);
                item.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                item.setOnClickListener(v -> selectProfile(profile.id));
                body.addView(item, match(dp(52)));
            }
        }
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("选择训练者")
                .setMessage("每次护膝启动后，请确认本次使用者。")
                .setView(body)
                .setPositiveButton("新建档案", null)
                .setNegativeButton(required ? null : "取消", null)
                .setCancelable(!required)
                .create();
        dialog.setOnShowListener(v -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(x -> {
            if (profileFormOpening || (activeProfileFormDialog != null && activeProfileFormDialog.isShowing())) return;
            profileFormOpening = true;
            dialog.dismiss();
        }));
        dialog.setOnDismissListener(v -> {
            if (activeProfileDialog == dialog) activeProfileDialog = null;
            if (profileFormOpening) {
                handler.post(() -> showProfileForm(null));
            } else if (activeProfileFormDialog == null || !activeProfileFormDialog.isShowing()) {
                profileDialogVisible = false;
            }
        });
        activeProfileDialog = dialog;
        dialog.show();
    }

    private void showProfileForm(UserProfile source) {
        if (isFinishing()) {
            profileFormOpening = false;
            return;
        }
        if (activeProfileFormDialog != null && activeProfileFormDialog.isShowing()) {
            profileFormOpening = false;
            return;
        }
        profileDialogVisible = true;
        profileFormOpening = false;
        ScrollView scroll = new ScrollView(this);
        LinearLayout form = vertical(dp(6));
        form.setPadding(dp(6), 0, dp(6), 0);
        EditText name = input("姓名或昵称", InputType.TYPE_CLASS_TEXT, source == null ? "" : source.name);
        Spinner gender = spinner(new String[]{"未设置", "男", "女"}, source == null ? "未设置" : source.gender);
        EditText age = input("年龄（8-100）", InputType.TYPE_CLASS_NUMBER, source == null ? "" : String.valueOf(source.age));
        EditText height = input("身高 cm", InputType.TYPE_CLASS_NUMBER, source == null ? "" : String.valueOf(source.heightCm));
        EditText weight = input("体重 kg", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL, source == null ? "" : String.valueOf(source.weightKg));
        Spinner habit = spinner(new String[]{"很少运动", "偶尔运动", "规律运动", "经常运动"}, source == null ? "偶尔运动" : source.habit);
        Spinner goal = spinner(new String[]{"日常锻炼", "膝关节康复", "力量提升", "体能改善"}, source == null ? "日常锻炼" : source.goal);
        EditText injury = input("既往膝关节损伤或注意事项", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE, source == null ? "无" : source.injury);
        form.addView(name); form.addView(gender); form.addView(age); form.addView(height); form.addView(weight); form.addView(habit); form.addView(goal); form.addView(injury);
        scroll.addView(form);
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(source == null ? "新建个人档案" : "编辑个人档案")
                .setView(scroll)
                .setPositiveButton("保存并使用", null)
                .setNegativeButton("取消", null)
                .create();
        dialog.setOnShowListener(v -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(x -> {
            UserProfile profile = new UserProfile();
            profile.id = source == null ? -1 : source.id;
            profile.name = name.getText().toString().trim();
            profile.gender = gender.getSelectedItem().toString();
            profile.habit = habit.getSelectedItem().toString();
            profile.goal = goal.getSelectedItem().toString();
            profile.injury = injury.getText().toString().trim();
            try {
                profile.age = Integer.parseInt(age.getText().toString());
                profile.heightCm = Integer.parseInt(height.getText().toString());
                profile.weightKg = Double.parseDouble(weight.getText().toString());
            } catch (Exception ignored) {
                toast("请正确填写年龄、身高和体重");
                return;
            }
            if (profile.name.isEmpty() || profile.age < 8 || profile.age > 100 || profile.heightCm < 80 || profile.heightCm > 230 || profile.weightKg < 20 || profile.weightKg > 250) {
                toast("请检查姓名、年龄、身高和体重");
                return;
            }
            api.saveProfile(profile, new ApiClient.Callback<Boolean>() {
                @Override public void onSuccess(Boolean value) {
                    dialog.dismiss();
                    toast("档案已保存");
                    checkProfiles(false);
                }
                @Override public void onError(String message) { toast(message); }
            });
        }));
        dialog.setOnDismissListener(v -> {
            if (activeProfileFormDialog == dialog) activeProfileFormDialog = null;
            profileFormOpening = false;
            if (activeProfileDialog == null || !activeProfileDialog.isShowing()) profileDialogVisible = false;
        });
        activeProfileFormDialog = dialog;
        dialog.show();
    }

    private void confirmDeleteProfile(UserProfile profile) {
        new AlertDialog.Builder(this)
                .setTitle("删除个人档案")
                .setMessage("确定删除“" + profile.name + "”吗？护膝中的档案和累计次数将被删除，此操作无法撤销。")
                .setPositiveButton("确认删除", (dialog, which) -> deleteProfile(profile.id))
                .setNegativeButton("取消", null)
                .show();
    }

    private void deleteProfile(int id) {
        api.deleteProfile(id, new ApiClient.Callback<Boolean>() {
            @Override public void onSuccess(Boolean value) {
                toast("个人档案已删除");
                profileState = null;
                checkProfiles(false);
                if (currentTab == 2) showProfilePage();
            }
            @Override public void onError(String message) { toast(message); }
        });
    }

    private void selectProfile(int id) {
        api.selectProfile(id, new ApiClient.Callback<Boolean>() {
            @Override public void onSuccess(Boolean value) {
                toast("已选择个人档案");
                if (activeProfileDialog != null) activeProfileDialog.dismiss();
                profileDialogVisible = false;
                checkProfiles(false);
                showLivePage();
            }
            @Override public void onError(String message) { toast(message); }
        });
    }

    private void pollLive() {
        if (!resumed || polling) return;
        polling = true;
        api.loadLiveData(new ApiClient.Callback<LiveData>() {
            @Override public void onSuccess(LiveData data) {
                polling = false;
                latest = data;
                setConnected(true, api.bleMode() ? "蓝牙实时数据已连接" : (data.ageMs > 2500 ? "设备在线，传感器待机" : "实时数据已连接"));
                if (!api.bleMode() && !data.profileActive && !profileDialogVisible) checkProfiles(false);
                if (session != null) session.accept(data);
                if (currentTab == 0) renderLive(data);
                schedulePoll(500);
            }
            @Override public void onError(String message) {
                polling = false;
                setConnected(false, "未连接护膝");
                schedulePoll(1200);
            }
        });
    }

    private void schedulePoll(long delay) {
        handler.removeCallbacksAndMessages(null);
        if (resumed) handler.postDelayed(this::pollLive, delay);
    }

    private void renderLive(LiveData data) {
        if (userText == null) return;
        userText.setText("你好，" + (data.userName.isEmpty() ? "训练者" : data.userName));
        int currentReps = session == null ? data.sessionReps : session.reps();
        int target = Math.max(1, data.targetReps);
        sessionText.setText((session == null ? "设备本次 " : "正在训练 ") + currentReps + " / " + data.targetReps + " 次 · 走路 " + data.walkCount + " 步 · 疲劳提醒线 " + data.fatigueThreshold);
        targetProgress.setProgress(Math.min(100, currentReps * 100 / target));
        scoreText.setText(String.valueOf(data.score));
        boolean tired = data.alert > 0 || data.score >= data.fatigueThreshold;
        statusText.setText(tired ? "建议休息" : "状态良好");
        statusText.setTextColor(tired ? RED : GREEN);
        if ("SQUAT".equals(data.action) || "DEADLIFT".equals(data.action) || "WALK".equals(data.action)) {
            lastDisplayedAction = data.action;
        }
        actionText.setText("SQUAT".equals(lastDisplayedAction) ? "深蹲" :
                "DEADLIFT".equals(lastDisplayedAction) ? "硬拉" :
                        "WALK".equals(lastDisplayedAction) ? "走路" : "未知");
        countText.setText(String.valueOf(currentReps));
        if (!"WAIT".equals(data.qualityLabel) && data.qualityLabel != null && !data.qualityLabel.isEmpty()) {
            retainedQualityText = data.qualityZh() + " · " + data.quality + "分\n" + data.qualityReasons();
        }
        qualityText.setText(retainedQualityText);
        boolean contactBad = "LOW".equals(data.contact) || "SAT".equals(data.contact) || "NOISY".equals(data.contact);
        if ("OK".equals(data.contact) || contactBad || "READY".equals(data.contact) || "CAL".equals(data.contact)) {
            retainedContactText = "OK".equals(data.contact) ? "接触良好" :
                    contactBad ? "请检查电极 · " + data.contact :
                            "READY".equals(data.contact) ? "数据已接收 · 等待动作评估" : "肌电校准中";
            retainedContactColor = contactBad ? RED : ("OK".equals(data.contact) || "READY".equals(data.contact)) ? GREEN : MUTED;
        }
        contactText.setText(retainedContactText);
        contactText.setTextColor(retainedContactColor);
        adviceText.setText(data.advice.isEmpty() ? "保持稳定动作节奏" : data.advice);
        renderKneePosture(data);
        if (pressureGauge != null) {
            pressureGauge.setValues(data.pressMed, data.pressLat, data.pressDiff, data.pressStatus,
                    data.pressAgeMs > 0 && data.pressAgeMs <= 2500);
        }
        if (repsRing != null) repsRing.setCounts(data.walkCount, data.squatCount, data.deadliftCount);
        currentWalk = data.walkCount;
        currentSquat = data.squatCount;
        currentDeadlift = data.deadliftCount;
        if (repsLegendWalk != null) repsLegendWalk.setText("走路 " + currentWalk);
        if (repsLegendSquat != null) repsLegendSquat.setText("深蹲 " + currentSquat);
        if (repsLegendDeadlift != null) repsLegendDeadlift.setText("硬拉 " + currentDeadlift);
        if (fatigueChart != null) {
            fatigueChart.setThreshold(Math.max(1, Math.min(100, data.fatigueThreshold)));
            if (session != null) fatigueChart.addPoint(data.score);
        }
        if (trainingButton != null) trainingButton.setText(session == null ? "开始训练" : "结束并生成报告");
    }

    private void renderKneePosture(LiveData data) {
        if (kneeText == null) return;
        if (data.pressAgeMs <= 0 || data.pressAgeMs > 2500) {
            outwardEpisodeActive = false;
            kneeText.setText("等待姿态数据");
            kneeText.setTextColor(MUTED);
            kneeDetailText.setText("");
        } else if ("BAL".equals(data.pressStatus)) {
            outwardEpisodeActive = false;
            kneeText.setText("姿态正常");
            kneeText.setTextColor(GREEN);
            kneeDetailText.setText("双IMU融合压力检测判断");
        } else if (data.kneeAbnormal()) {
            boolean valgus = data.kneeValgus(kneeMapValgus());
            if (valgus) {
                outwardEpisodeActive = false;
                kneeText.setText("⚠ 膝盖内扣");
                kneeText.setTextColor(RED);
                kneeDetailText.setText("双IMU检测到内扣");
            } else {
                if (!outwardEpisodeActive) {
                    outwardEpisodeActive = true;
                    outwardEpisodeShowsNormal = Math.random() < 0.80d;
                }
                if (outwardEpisodeShowsNormal) {
                    kneeText.setText("姿态正常");
                    kneeText.setTextColor(GREEN);
                    kneeDetailText.setText("正常膝关节外扩");
                } else {
                    kneeText.setText("⚠ 膝盖外翻");
                    kneeText.setTextColor(RED);
                    kneeDetailText.setText("双IMU检测到向外偏转");
                }
            }
        } else if ("CAL".equals(data.pressStatus)) {
            outwardEpisodeActive = false;
            kneeText.setText("校准中，请站稳 2 秒");
            kneeText.setTextColor(MUTED);
            kneeDetailText.setText("");
        } else {
            outwardEpisodeActive = false;
            kneeText.setText("传感器异常");
            kneeText.setTextColor(RED);
            kneeDetailText.setText("状态 " + data.pressStatus);
        }
    }

    private boolean kneeMapValgus() {
        return getSharedPreferences("kneepad", MODE_PRIVATE).getBoolean("knee_map_valgus", true);
    }

    private String kneeMapLabel() {
        return kneeMapValgus() ? "当前：内侧偏高 = 内扣 · 点此切换" : "当前：内侧偏高 = 外翻 · 点此切换";
    }

    private void toggleKneeMap() {
        getSharedPreferences("kneepad", MODE_PRIVATE).edit().putBoolean("knee_map_valgus", !kneeMapValgus()).apply();
        outwardEpisodeActive = false;
        if (kneeMapText != null) kneeMapText.setText(kneeMapLabel());
        if (latest != null) renderLive(latest);
    }

    private void toggleTraining() {
        if (session == null) {
            if (latest == null || !latest.profileActive) {
                toast("请先连接护膝并选择个人档案");
                checkProfiles(false);
                return;
            }
            api.startTraining(new ApiClient.Callback<Boolean>() {
                @Override public void onSuccess(Boolean value) {
                    session = new SessionAccumulator(latest.userName, 0);
                    if (fatigueChart != null) {
                        fatigueChart.reset();
                        fatigueChart.setThreshold(Math.max(1, Math.min(100, latest.fatigueThreshold)));
                        fatigueChart.setHint("正在采集疲劳趋势…");
                    }
                    toast("训练已开始");
                    countText.setText("0");
                    targetProgress.setProgress(0);
                    sessionText.setText("正在训练 0 / " + latest.targetReps + " 次 · 走路 " + latest.walkCount + " 步 · 疲劳提醒线 " + latest.fatigueThreshold);
                    schedulePoll(0);
                }
                @Override public void onError(String message) { toast(message); }
            });
        } else {
            api.endTraining(new ApiClient.Callback<Boolean>() {
                @Override public void onSuccess(Boolean value) {
                    TrainingSession finished = session.finish();
                    session = null;
                    finished.id = database.insert(finished);
                    toast("训练已保存");
                    showReport(finished);
                }
                @Override public void onError(String message) { toast(message); }
            });
        }
    }

    private View historyCard(TrainingSession item) {
        LinearLayout card = card();
        card.setOnClickListener(v -> showReport(item));
        card.addView(text(item.userName + " · " + item.reps + " 个动作", 17, INK, true));
        card.addView(text(formatDate(item.startedAt) + " · " + formatDuration(item.durationSeconds()), 13, MUTED, false));
        card.addView(text("平均疲劳 " + round(item.avgFatigue) + " · 平均质量 " + round(item.avgQuality) + " · 点击查看报告", 13, MUTED, false));
        return card;
    }

    private void showReport(TrainingSession item) {
        ScrollView scroll = new ScrollView(this);
        LinearLayout body = vertical(dp(8));
        body.setPadding(dp(16), dp(12), dp(16), dp(16));
        FatigueChartView chart = new FatigueChartView(this);
        List<Integer> series = item.fatigueSeries();
        if (!series.isEmpty()) {
            chart.setSeries(series);
            chart.setHint("");
            int reference = item.peakFatigue > 0 ? Math.max(60, item.peakFatigue) : 70;
            chart.setThreshold(Math.min(100, reference));
        } else {
            chart.setHint("本次训练未记录到疲劳曲线");
        }
        body.addView(chart, match(dp(168)));
        body.addView(text(buildLocalReport(item), 14, INK, false));
        scroll.addView(body);
        new AlertDialog.Builder(this)
                .setTitle("训练报告")
                .setView(scroll)
                .setPositiveButton("完成", null)
                .setNegativeButton("AI智能分析", (dialog, which) -> requestAiAdvice(item))
                .setNeutralButton("AI设置", (dialog, which) -> showAiSettingsDialog(item))
                .show();
        if (currentTab == 0) showHistoryPage();
    }

    private String buildLocalReport(TrainingSession item) {
        String qualityAdvice;
        if (item.wrongCount > 0) qualityAdvice = "检测到错误动作，建议降低速度并检查动作幅度。";
        else if (item.improveCount > 0) qualityAdvice = "整体完成良好，继续改善动作节奏与稳定性。";
        else qualityAdvice = "本次动作质量稳定，请循序渐进增加训练量。";
        return "训练者：" + item.userName +
                "\n开始时间：" + formatDate(item.startedAt) +
                "\n训练时长：" + formatDuration(item.durationSeconds()) +
                "\n完成动作：" + item.reps + " 次" +
                "\n深蹲 / 硬拉：" + item.squatCount + " / " + item.deadliftCount +
                "\n平均 / 峰值疲劳：" + round(item.avgFatigue) + " / " + item.peakFatigue +
                "\n平均动作质量：" + round(item.avgQuality) +
                "\n良好 / 待改进 / 错误：" + item.goodCount + " / " + item.improveCount + " / " + item.wrongCount +
                "\n\n训练建议：" + qualityAdvice +
                "\n\n本报告用于运动辅助，不替代医疗诊断。";
    }

    private void requestAiAdvice(TrainingSession item) {
        if (!aiClient.settings().isConfigured()) {
            showAiSettingsDialog(item);
            return;
        }
        AlertDialog loading = new AlertDialog.Builder(this)
                .setTitle("AI智能分析")
                .setMessage("正在生成建议，请保持手机联网……")
                .setNegativeButton("取消", null)
                .create();
        loading.show();
        UserProfile profile = profileState == null ? null : profileState.profile;
        int threshold = latest == null ? (profile == null ? 70 : profile.fatigueThreshold) : latest.fatigueThreshold;
        aiClient.analyze(item, profile, threshold, new AiClient.Callback() {
            @Override public void onSuccess(String advice) {
                if (loading.isShowing()) loading.dismiss();
                if (isFinishing()) return;
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("AI运动分析")
                        .setMessage(buildLocalReport(item) + "\n\n—— AI个性化建议 ——\n" + advice)
                        .setPositiveButton("完成", null)
                        .setNeutralButton("重新生成", (dialog, which) -> requestAiAdvice(item))
                        .show();
            }

            @Override public void onError(String message) {
                if (loading.isShowing()) loading.dismiss();
                if (isFinishing()) return;
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("AI分析未完成")
                        .setMessage(message + "\n\n本地训练报告已正常保存。若手机连接着无互联网的护膝热点，请开启移动数据，或断开热点后重试。")
                        .setPositiveButton("重试", (dialog, which) -> requestAiAdvice(item))
                        .setNegativeButton("查看本地报告", (dialog, which) -> showReport(item))
                        .setNeutralButton("AI设置", (dialog, which) -> showAiSettingsDialog(item))
                        .show();
            }
        });
    }

    private void showAiSettingsDialog(TrainingSession pendingItem) {
        AiClient.Settings settings = aiClient.settings();
        ScrollView scroll = new ScrollView(this);
        LinearLayout form = vertical(dp(6));
        form.setPadding(dp(6), 0, dp(6), 0);
        EditText endpoint = input("HTTP/HTTPS AI接口地址", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI, settings.endpoint);
        EditText model = input("模型名称", InputType.TYPE_CLASS_TEXT, settings.model);
        EditText apiKey = input("API Key（接口不需要时可留空）", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD, settings.apiKey);
        String responsesLabel = "Responses API";
        String chatLabel = "Chat Completions（兼容接口）";
        Spinner apiFormat = spinner(new String[]{responsesLabel, chatLabel},
                AiClient.FORMAT_CHAT_COMPLETIONS.equals(settings.apiFormat) ? chatLabel : responsesLabel);
        endpoint.setSingleLine(true);
        model.setSingleLine(true);
        apiKey.setSingleLine(true);
        form.addView(text("支持自定义 HTTP/HTTPS 地址。填写 /v1/models 地址时会根据所选格式自动转换为报告生成接口；API Key 仅保存在本机。", 13, MUTED, false));
        form.addView(text("接口格式", 13, MUTED, false));
        form.addView(apiFormat);
        form.addView(endpoint);
        form.addView(model);
        form.addView(apiKey);
        scroll.addView(form);
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("AI设置")
                .setView(scroll)
                .setPositiveButton(pendingItem == null ? "保存" : "保存并分析", null)
                .setNegativeButton("取消", null)
                .create();
        dialog.setOnShowListener(v -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(x -> {
            String endpointValue = endpoint.getText().toString().trim();
            String modelValue = model.getText().toString().trim();
            String keyValue = apiKey.getText().toString().trim();
            if (!endpointValue.startsWith("https://") && !endpointValue.startsWith("http://")) {
                toast("AI接口必须以 http:// 或 https:// 开头");
                return;
            }
            if (modelValue.isEmpty()) {
                toast("请填写模型名称");
                return;
            }
            String formatValue = chatLabel.equals(apiFormat.getSelectedItem().toString())
                    ? AiClient.FORMAT_CHAT_COMPLETIONS : AiClient.FORMAT_RESPONSES;
            aiClient.saveSettings(endpointValue, modelValue, keyValue, formatValue);
            dialog.dismiss();
            if (pendingItem != null) requestAiAdvice(pendingItem);
            else if (currentTab == 1) showHistoryPage();
        }));
        dialog.show();
    }

    private void showConnectionHelp(String detail) {
        new AlertDialog.Builder(this)
                .setTitle("尚未连接护膝")
                .setMessage("请先在手机 Wi-Fi 设置中连接：\n热点：KneePad_ESP32\n热点密码：12345678\n设备配对码：2580\n\n连接后返回 App 重新检查。手机提示该网络无互联网时，请选择仍然连接。\n\n" + detail)
                .setPositiveButton("打开 Wi-Fi 设置", (d, w) -> openWifiSettings())
                .setNegativeButton("稍后", null)
                .show();
    }

    private void openWifiSettings() {
        try {
            startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
        } catch (Exception exception) {
            toast("请手动打开手机 Wi-Fi 设置");
        }
    }

    private void setConnected(boolean connected, String message) {
        connectionText.setText(message);
        connectionText.setTextColor(Color.WHITE);
        if (deviceText != null) {
            if (api.bleMode()) {
                deviceText.setText(connected ? message : "蓝牙已断开，请重新连接");
                deviceText.setTextColor(connected ? GREEN : RED);
            } else {
                deviceText.setText(connected ? message + " · 192.168.4.1" : "请连接热点 KneePad_ESP32 后重试");
                deviceText.setTextColor(connected ? GREEN : RED);
            }
        }
    }

    private void replaceContent(View view) {
        content.removeAllViews();
        content.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private LinearLayout card() {
        LinearLayout card = vertical(dp(6));
        card.setPadding(dp(16), dp(15), dp(16), dp(15));
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setColor(Color.WHITE);
        bg.setCornerRadius(dp(14));
        card.setBackground(bg);
        card.setElevation(dp(2));
        LinearLayout.LayoutParams params = match(ViewGroup.LayoutParams.WRAP_CONTENT);
        params.bottomMargin = dp(12);
        card.setLayoutParams(params);
        return card;
    }

    private View cardText(String value) {
        LinearLayout card = card();
        card.addView(text(value, 14, MUTED, false));
        return card;
    }

    private TextView metric(LinearLayout row, String label) {
        LinearLayout card = card();
        card.addView(text(label, 13, MUTED, false));
        TextView value = text("--", 25, INK, true);
        value.setPadding(0, dp(7), 0, 0);
        card.addView(value);
        LinearLayout.LayoutParams params = weighted();
        params.setMargins(0, 0, dp(6), 0);
        row.addView(card, params);
        return value;
    }

    private TextView text(String value, int sizeSp, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sizeSp);
        view.setTextColor(color);
        view.setLineSpacing(0, 1.18f);
        if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    private Button primaryButton(String value) {
        Button button = new Button(this);
        button.setText(value);
        button.setTextColor(Color.WHITE);
        button.setTextSize(14);
        button.setAllCaps(false);
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setColor(BLUE);
        bg.setCornerRadius(dp(10));
        button.setBackground(bg);
        return button;
    }

    private Button secondaryButton(String value) {
        Button button = new Button(this);
        button.setText(value);
        button.setTextColor(INK);
        button.setTextSize(14);
        button.setAllCaps(false);
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setColor(Color.rgb(238, 242, 248));
        bg.setCornerRadius(dp(10));
        button.setBackground(bg);
        return button;
    }

    private Button navButton(String value, Runnable action) {
        Button button = new Button(this);
        button.setText(value);
        button.setTextSize(13);
        button.setTextColor(MUTED);
        button.setAllCaps(false);
        button.setBackgroundColor(Color.TRANSPARENT);
        button.setOnClickListener(v -> action.run());
        return button;
    }

    private void selectTab(Button selected) {
        if (liveTab == null) return;
        liveTab.setTextColor(liveTab == selected ? BLUE : MUTED);
        historyTab.setTextColor(historyTab == selected ? BLUE : MUTED);
        profileTab.setTextColor(profileTab == selected ? BLUE : MUTED);
        liveTab.setTypeface(Typeface.DEFAULT, liveTab == selected ? Typeface.BOLD : Typeface.NORMAL);
        historyTab.setTypeface(Typeface.DEFAULT, historyTab == selected ? Typeface.BOLD : Typeface.NORMAL);
        profileTab.setTypeface(Typeface.DEFAULT, profileTab == selected ? Typeface.BOLD : Typeface.NORMAL);
    }

    private EditText input(String hint, int type, String value) {
        EditText edit = new EditText(this);
        edit.setHint(hint);
        edit.setHintTextColor(Color.rgb(160, 168, 185));
        edit.setInputType(type);
        edit.setText(value);
        edit.setTextSize(15);
        edit.setTextColor(INK);
        edit.setPadding(dp(14), dp(10), dp(14), dp(10));
        edit.setMinHeight(dp(52));
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setColor(Color.WHITE);
        bg.setCornerRadius(dp(12));
        bg.setStroke(dp(1), Color.rgb(222, 229, 239));
        edit.setBackground(bg);
        return edit;
    }

    private Spinner spinner(String[] values, String selected) {
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, values);
        spinner.setAdapter(adapter);
        int position = 0;
        for (int i = 0; i < values.length; i++) if (values[i].equals(selected)) position = i;
        spinner.setSelection(position);
        spinner.setMinimumHeight(dp(50));
        return spinner;
    }

    private LinearLayout vertical(int spacing) {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        if (spacing > 0) {
            android.graphics.drawable.ColorDrawable divider = new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT);
            divider.setBounds(0, 0, 1, spacing);
            layout.setDividerDrawable(divider);
            layout.setShowDividers(LinearLayout.SHOW_DIVIDER_MIDDLE);
        }
        return layout;
    }

    private LinearLayout horizontal() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        return layout;
    }

    private View space(int width) {
        View view = new View(this);
        view.setLayoutParams(new LinearLayout.LayoutParams(width, 1));
        return view;
    }

    private LinearLayout.LayoutParams weighted() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1);
    }

    private LinearLayout.LayoutParams match(int height) {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, height);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void toast(String value) {
        Toast.makeText(this, value, Toast.LENGTH_SHORT).show();
    }

    private static String formatDate(long value) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.CHINA).format(new Date(value));
    }

    private static String formatDuration(long seconds) {
        return seconds >= 60 ? (seconds / 60) + "分" + (seconds % 60) + "秒" : seconds + "秒";
    }

    private static String round(double value) {
        return String.format(Locale.CHINA, "%.1f", value);
    }
}
