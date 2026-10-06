package com.kneepad.app;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/**
 * 本地账号系统：注册 / 登录 / 登出。
 * 账号以「用户名 -> SHA-256 密码哈希」形式保存在手机本地 SharedPreferences，
 * 仅用于演示身份区分，密码不明文保存。
 */
final class AuthManager {
    private static final String PREFS = "kneepad_auth";
    private static final String KEY_USERS = "users";
    private static final String KEY_CURRENT = "current_user";

    private final SharedPreferences prefs;

    AuthManager(Context context) {
        prefs = context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    boolean isLoggedIn() {
        String current = prefs.getString(KEY_CURRENT, "");
        return current != null && !current.trim().isEmpty();
    }

    String currentUser() {
        return prefs.getString(KEY_CURRENT, "");
    }

    void logout() {
        prefs.edit().remove(KEY_CURRENT).apply();
    }

    /** 注册。成功返回 null，失败返回错误信息。 */
    String register(String username, String password) {
        username = username == null ? "" : username.trim();
        if (username.length() < 2) return "用户名至少 2 个字符";
        if (username.length() > 20) return "用户名不能超过 20 个字符";
        if (password == null || password.length() < 4) return "密码至少 4 位";
        if (password.length() > 32) return "密码不能超过 32 位";

        JSONObject users = loadUsers();
        if (users.has(username)) return "该用户名已被注册";
        try {
            users.put(username, sha256(password));
        } catch (Exception exception) {
            return "注册失败，请重试";
        }
        prefs.edit()
                .putString(KEY_USERS, users.toString())
                .putString(KEY_CURRENT, username)
                .apply();
        return null;
    }

    /** 登录。成功返回 null，失败返回错误信息。 */
    String login(String username, String password) {
        username = username == null ? "" : username.trim();
        if (username.isEmpty()) return "请输入用户名";
        if (password == null || password.isEmpty()) return "请输入密码";

        JSONObject users = loadUsers();
        String hash = users.optString(username, "");
        if (hash.isEmpty()) return "该用户不存在，请先注册";
        if (!sha256(password).equals(hash)) return "密码错误";
        prefs.edit().putString(KEY_CURRENT, username).apply();
        return null;
    }

    private JSONObject loadUsers() {
        String raw = prefs.getString(KEY_USERS, "{}");
        try {
            return new JSONObject(raw);
        } catch (Exception exception) {
            return new JSONObject();
        }
    }

    private static String sha256(String value) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] bytes = digest.digest(value.getBytes(StandardCharsets.UTF_8));
            StringBuilder sb = new StringBuilder();
            for (byte b : bytes) sb.append(String.format(java.util.Locale.US, "%02x", b));
            return sb.toString();
        } catch (Exception exception) {
            return String.valueOf(value.hashCode());
        }
    }
}
