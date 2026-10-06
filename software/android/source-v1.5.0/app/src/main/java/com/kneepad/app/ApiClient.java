package com.kneepad.app;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Handler;
import android.os.Looper;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class ApiClient {
    interface Callback<T> {
        void onSuccess(T value);
        void onError(String message);
    }

    private static final String BASE_URL = "http://192.168.4.1";
    private static final String PREFS = "kneepad_connection";
    private static final String KEY_PAIR_CODE = "pair_code";
    private final Context context;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    ApiClient(Context context) {
        this.context = context.getApplicationContext();
    }

    void loadProfiles(Callback<ProfileState> callback) {
        execute(() -> {
            JSONObject root = new JSONObject(request("GET", "/api/profile", null));
            ProfileState state = new ProfileState();
            state.active = root.optBoolean("active", false);
            JSONArray items = root.optJSONArray("profiles");
            if (items != null) {
                for (int i = 0; i < items.length(); i++) {
                    state.profiles.add(UserProfile.fromJson(items.getJSONObject(i)));
                }
            }
            JSONObject active = root.optJSONObject("profile");
            if (active != null) state.profile = UserProfile.fromJson(active);
            return state;
        }, callback);
    }

    String pairCode() {
        String saved = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_PAIR_CODE, "");
        // 设备默认配对码 2580 自动使用，用户无需手动输入
        return saved.isEmpty() ? "2580" : saved;
    }

    void clearPairCode() {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(KEY_PAIR_CODE).apply();
    }

    void verifyPairCode(String code, Callback<Boolean> callback) {
        Map<String, String> body = new LinkedHashMap<>();
        body.put("pair_code", code == null ? "" : code.trim());
        execute(() -> {
            boolean ok = new JSONObject(request("POST", "/api/pair/verify", body)).optBoolean("ok");
            if (ok) context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY_PAIR_CODE, code.trim()).apply();
            return ok;
        }, callback);
    }

    void selectProfile(int id, Callback<Boolean> callback) {
        Map<String, String> body = authorizedBody();
        body.put("id", String.valueOf(id));
        execute(() -> new JSONObject(request("POST", "/api/profile/select", body)).optBoolean("ok"), callback);
    }

    void saveProfile(UserProfile profile, Callback<Boolean> callback) {
        Map<String, String> body = authorizedBody();
        body.put("id", String.valueOf(profile.id));
        body.put("name", profile.name);
        body.put("gender", profile.gender);
        body.put("age", String.valueOf(profile.age));
        body.put("height", String.valueOf(profile.heightCm));
        body.put("weight", String.valueOf(profile.weightKg));
        body.put("habit", profile.habit);
        body.put("goal", profile.goal);
        body.put("injury", profile.injury);
        execute(() -> new JSONObject(request("POST", "/api/profile/save", body)).optBoolean("ok"), callback);
    }

    void deleteProfile(int id, Callback<Boolean> callback) {
        Map<String, String> body = authorizedBody();
        body.put("id", String.valueOf(id));
        execute(() -> new JSONObject(request("POST", "/api/profile/delete", body)).optBoolean("ok"), callback);
    }

    void startTraining(Callback<Boolean> callback) {
        execute(() -> new JSONObject(request("POST", "/api/training/start", authorizedBody())).optBoolean("ok"), callback);
    }

    void endTraining(Callback<Boolean> callback) {
        execute(() -> new JSONObject(request("POST", "/api/training/end", authorizedBody())).optBoolean("ok"), callback);
    }

    void loadLiveData(Callback<LiveData> callback) {
        execute(() -> LiveData.fromJson(new JSONObject(request("GET", "/api", null))), callback);
    }

    void shutdown() {
        executor.shutdownNow();
    }

    private interface Work<T> {
        T run() throws Exception;
    }

    private <T> void execute(Work<T> work, Callback<T> callback) {
        executor.execute(() -> {
            try {
                T result = work.run();
                mainHandler.post(() -> callback.onSuccess(result));
            } catch (Exception exception) {
                String message = exception.getMessage();
                if (message == null || message.trim().isEmpty()) message = "无法连接护膝设备";
                String finalMessage = message;
                mainHandler.post(() -> callback.onError(finalMessage));
            }
        });
    }

    private String request(String method, String path, Map<String, String> fields) throws Exception {
        URL url = new URL(BASE_URL + path);
        HttpURLConnection connection = openWifiConnection(url);
        connection.setRequestMethod(method);
        connection.setConnectTimeout(1800);
        connection.setReadTimeout(1800);
        connection.setUseCaches(false);
        connection.setRequestProperty("Accept", "application/json");
        if (fields != null) {
            connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
            byte[] payload = encode(fields).getBytes(StandardCharsets.UTF_8);
            connection.setFixedLengthStreamingMode(payload.length);
            try (OutputStream output = connection.getOutputStream()) {
                output.write(payload);
            }
        }
        int status = connection.getResponseCode();
        InputStream stream = status >= 200 && status < 300 ? connection.getInputStream() : connection.getErrorStream();
        String response = readAll(stream);
        connection.disconnect();
        if (status < 200 || status >= 300) {
            try {
                String error = new JSONObject(response).optString("error", "设备返回错误 " + status);
                throw new IllegalStateException(error);
            } catch (org.json.JSONException ignored) {
                throw new IllegalStateException("设备返回错误 " + status);
            }
        }
        return response;
    }

    private HttpURLConnection openWifiConnection(URL url) throws Exception {
        ConnectivityManager manager = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (manager != null) {
            for (Network network : manager.getAllNetworks()) {
                NetworkCapabilities capabilities = manager.getNetworkCapabilities(network);
                if (capabilities != null && capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                    return (HttpURLConnection) network.openConnection(url);
                }
            }
        }
        return (HttpURLConnection) url.openConnection();
    }

    private Map<String, String> authorizedBody() {
        Map<String, String> body = new LinkedHashMap<>();
        body.put("pair_code", pairCode());
        return body;
    }

    private static String encode(Map<String, String> fields) throws Exception {
        StringBuilder result = new StringBuilder();
        for (Map.Entry<String, String> entry : fields.entrySet()) {
            if (result.length() > 0) result.append('&');
            result.append(URLEncoder.encode(entry.getKey(), "UTF-8"));
            result.append('=');
            result.append(URLEncoder.encode(entry.getValue() == null ? "" : entry.getValue(), "UTF-8"));
        }
        return result.toString();
    }

    private static String readAll(InputStream stream) throws Exception {
        if (stream == null) return "";
        StringBuilder result = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) result.append(line);
        }
        return result.toString();
    }
}
