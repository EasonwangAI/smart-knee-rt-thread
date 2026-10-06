package com.kneepad.app;

import android.content.Context;
import android.content.SharedPreferences;
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
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Demo AI client. It deliberately does not reuse ApiClient because device traffic
 * is pinned to the ESP32 Wi-Fi network while AI traffic needs Internet access.
 */
final class AiClient {
    static final String FORMAT_RESPONSES = "responses";
    static final String FORMAT_CHAT_COMPLETIONS = "chat_completions";

    interface Callback {
        void onSuccess(String advice);
        void onError(String message);
    }

    static final class Settings {
        String endpoint;
        String model;
        String apiKey;
        String apiFormat;

        boolean isConfigured() {
            return endpoint != null &&
                    (endpoint.startsWith("https://") || endpoint.startsWith("http://")) &&
                    model != null && !model.trim().isEmpty();
        }
    }

    private static final String PREFS = "kneepad_ai_demo";
    private static final String KEY_ENDPOINT = "endpoint";
    private static final String KEY_MODEL = "model";
    private static final String KEY_API_KEY = "api_key";
    private static final String KEY_API_FORMAT = "api_format";
    private static final String DEFAULT_ENDPOINT = "https://api.openai.com/v1/responses";
    private static final String DEFAULT_MODEL = "gpt-5.6-luna";
    private static final String SYSTEM_PROMPT =
            "你是一名谨慎的运动训练助手，只能根据给出的训练统计生成运动辅助建议。" +
            "不得诊断疾病，不得声称替代医生或康复治疗师。" +
            "如果峰值疲劳达到或超过个人提醒线，不得建议增加训练量。" +
            "如果错误动作较多，应优先建议降低强度、纠正动作和休息。" +
            "请用简洁中文输出：一段表现总结，加三条编号建议，最后固定写“本建议仅用于运动辅助，不替代医疗诊断。”";

    private final SharedPreferences preferences;
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    AiClient(Context context) {
        preferences = context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    Settings settings() {
        Settings value = new Settings();
        value.endpoint = preferences.getString(KEY_ENDPOINT, DEFAULT_ENDPOINT);
        value.model = preferences.getString(KEY_MODEL, DEFAULT_MODEL);
        value.apiKey = preferences.getString(KEY_API_KEY, "");
        value.apiFormat = preferences.getString(KEY_API_FORMAT, "");
        if (value.apiFormat == null || value.apiFormat.isEmpty()) {
            value.apiFormat = value.endpoint != null && value.endpoint.contains("/chat/completions")
                    ? FORMAT_CHAT_COMPLETIONS : FORMAT_RESPONSES;
        }
        return value;
    }

    void saveSettings(String endpoint, String model, String apiKey, String apiFormat) {
        preferences.edit()
                .putString(KEY_ENDPOINT, endpoint == null ? "" : endpoint.trim())
                .putString(KEY_MODEL, model == null ? "" : model.trim())
                .putString(KEY_API_KEY, apiKey == null ? "" : apiKey.trim())
                .putString(KEY_API_FORMAT, FORMAT_CHAT_COMPLETIONS.equals(apiFormat)
                        ? FORMAT_CHAT_COMPLETIONS : FORMAT_RESPONSES)
                .apply();
    }

    void analyze(TrainingSession session, UserProfile profile, int fatigueThreshold, Callback callback) {
        Settings config = settings();
        if (!config.isConfigured()) {
            callback.onError("请先完成 AI 设置");
            return;
        }
        executor.execute(() -> {
            try {
                String advice = request(config, buildPrompt(session, profile, fatigueThreshold));
                mainHandler.post(() -> callback.onSuccess(advice));
            } catch (Exception exception) {
                String message = exception.getMessage();
                if (message == null || message.trim().isEmpty()) message = "AI 服务暂时不可用";
                String finalMessage = message;
                mainHandler.post(() -> callback.onError(finalMessage));
            }
        });
    }

    void shutdown() {
        executor.shutdownNow();
    }

    private String request(Settings config, String userPrompt) throws Exception {
        String resolvedEndpoint = resolveGenerationEndpoint(config);
        URL url = new URL(resolvedEndpoint);
        if (!"https".equalsIgnoreCase(url.getProtocol()) && !"http".equalsIgnoreCase(url.getProtocol())) {
            throw new IllegalArgumentException("AI 接口必须使用 HTTP 或 HTTPS");
        }
        HttpURLConnection connection = (HttpURLConnection) url.openConnection();
        connection.setRequestMethod("POST");
        connection.setConnectTimeout(12000);
        connection.setReadTimeout(30000);
        connection.setUseCaches(false);
        connection.setDoOutput(true);
        connection.setRequestProperty("Accept", "application/json");
        connection.setRequestProperty("Content-Type", "application/json; charset=UTF-8");
        if (config.apiKey != null && !config.apiKey.trim().isEmpty()) {
            connection.setRequestProperty("Authorization", "Bearer " + config.apiKey.trim());
        }

        boolean chatCompletions = FORMAT_CHAT_COMPLETIONS.equals(config.apiFormat);
        JSONObject body = chatCompletions
                ? chatCompletionsBody(config.model, userPrompt)
                : responsesBody(config.model, userPrompt);
        byte[] payload = body.toString().getBytes(StandardCharsets.UTF_8);
        connection.setFixedLengthStreamingMode(payload.length);
        try (OutputStream output = connection.getOutputStream()) {
            output.write(payload);
        }

        int status = connection.getResponseCode();
        InputStream stream = status >= 200 && status < 300
                ? connection.getInputStream() : connection.getErrorStream();
        String raw = readAll(stream);
        connection.disconnect();
        JSONObject response = new JSONObject(raw.isEmpty() ? "{}" : raw);
        if (status < 200 || status >= 300) {
            JSONObject error = response.optJSONObject("error");
            String detail = error == null ? "" : error.optString("message", "");
            throw new IllegalStateException(detail.isEmpty() ? "AI 接口返回错误 " + status : detail);
        }

        String text = chatCompletions ? parseChatResponse(response) : parseResponsesResponse(response);
        if (text.isEmpty()) throw new IllegalStateException("AI 没有返回可显示的建议");
        return text;
    }

    private static String resolveGenerationEndpoint(Settings config) {
        String endpoint = config.endpoint == null ? "" : config.endpoint.trim();
        if (endpoint.endsWith("/models")) {
            endpoint = endpoint.substring(0, endpoint.length() - "/models".length());
            endpoint += FORMAT_CHAT_COMPLETIONS.equals(config.apiFormat)
                    ? "/chat/completions" : "/responses";
        }
        return endpoint;
    }

    private static JSONObject responsesBody(String model, String userPrompt) throws Exception {
        JSONObject body = new JSONObject();
        body.put("model", model);
        body.put("store", false);
        body.put("max_output_tokens", 500);
        JSONArray input = new JSONArray();
        input.put(new JSONObject().put("role", "system").put("content", SYSTEM_PROMPT));
        input.put(new JSONObject().put("role", "user").put("content", userPrompt));
        body.put("input", input);
        return body;
    }

    private static JSONObject chatCompletionsBody(String model, String userPrompt) throws Exception {
        JSONObject body = new JSONObject();
        body.put("model", model);
        body.put("max_tokens", 500);
        JSONArray messages = new JSONArray();
        messages.put(new JSONObject().put("role", "system").put("content", SYSTEM_PROMPT));
        messages.put(new JSONObject().put("role", "user").put("content", userPrompt));
        body.put("messages", messages);
        return body;
    }

    private static String parseResponsesResponse(JSONObject response) {
        String direct = response.optString("output_text", "").trim();
        if (!direct.isEmpty()) return direct;
        JSONArray output = response.optJSONArray("output");
        if (output == null) return "";
        StringBuilder result = new StringBuilder();
        for (int i = 0; i < output.length(); i++) {
            JSONObject item = output.optJSONObject(i);
            JSONArray content = item == null ? null : item.optJSONArray("content");
            if (content == null) continue;
            for (int j = 0; j < content.length(); j++) {
                JSONObject part = content.optJSONObject(j);
                if (part == null) continue;
                String text = part.optString("text", "").trim();
                if (!text.isEmpty()) {
                    if (result.length() > 0) result.append('\n');
                    result.append(text);
                }
            }
        }
        return result.toString();
    }

    private static String parseChatResponse(JSONObject response) {
        JSONArray choices = response.optJSONArray("choices");
        if (choices == null || choices.length() == 0) return "";
        JSONObject choice = choices.optJSONObject(0);
        JSONObject message = choice == null ? null : choice.optJSONObject("message");
        return message == null ? "" : message.optString("content", "").trim();
    }

    private static String buildPrompt(TrainingSession session, UserProfile profile, int fatigueThreshold) {
        int threshold = fatigueThreshold > 0 ? fatigueThreshold : 70;
        StringBuilder prompt = new StringBuilder("请分析下面这次训练：\n");
        prompt.append("训练时长：").append(session.durationSeconds()).append("秒\n")
                .append("完成动作：").append(session.reps).append("次\n")
                .append("深蹲/硬拉：").append(session.squatCount).append('/').append(session.deadliftCount).append("次\n")
                .append("平均疲劳：").append(round(session.avgFatigue)).append("\n")
                .append("峰值疲劳：").append(session.peakFatigue).append("\n")
                .append("个人疲劳提醒线：").append(threshold).append("\n")
                .append("平均动作质量：").append(round(session.avgQuality)).append("\n")
                .append("良好/待改进/错误：").append(session.goodCount).append('/')
                .append(session.improveCount).append('/').append(session.wrongCount).append("次\n");
        if (profile != null) {
            prompt.append("年龄：").append(profile.age).append("岁\n")
                    .append("运动习惯：").append(profile.habit).append("\n")
                    .append("训练目标：").append(profile.goal).append("\n")
                    .append("损伤或注意事项：").append(profile.injury).append("\n");
        }
        prompt.append("不要复述姓名等身份信息，给出简短、保守、可执行的建议。");
        return prompt.toString();
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

    private static String round(double value) {
        return String.format(java.util.Locale.CHINA, "%.1f", value);
    }
}
