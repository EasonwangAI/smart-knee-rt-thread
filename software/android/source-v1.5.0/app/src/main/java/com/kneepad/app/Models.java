package com.kneepad.app;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

final class ProfileState {
    boolean active;
    final List<UserProfile> profiles = new ArrayList<>();
    UserProfile profile;
}

final class UserProfile {
    int id = -1;
    String name = "";
    String gender = "未设置";
    int age;
    int heightCm;
    double weightKg;
    String habit = "偶尔运动";
    String goal = "日常锻炼";
    String injury = "无";
    int totalReps;
    double bmi;
    int fatigueThreshold = 70;
    int targetReps = 10;

    static UserProfile fromJson(JSONObject json) {
        UserProfile profile = new UserProfile();
        profile.id = json.optInt("id", -1);
        profile.name = json.optString("name", "");
        profile.gender = json.optString("gender", "未设置");
        profile.age = json.optInt("age", 0);
        profile.heightCm = json.optInt("height_cm", 0);
        profile.weightKg = json.optDouble("weight_kg", 0);
        profile.habit = json.optString("exercise_habit", "偶尔运动");
        profile.goal = json.optString("goal", "日常锻炼");
        profile.injury = json.optString("injury", "无");
        profile.totalReps = json.optInt("total_reps", 0);
        profile.bmi = json.optDouble("bmi", 0);
        profile.fatigueThreshold = json.optInt("fatigue_threshold", 70);
        profile.targetReps = json.optInt("target_reps", 10);
        return profile;
    }
}

final class LiveData {
    boolean profileActive;
    boolean trainingActive;
    int score;
    String action = "UNKNOWN";
    int count;
    int alert;
    int quality;
    String qualityLabel = "WAIT";
    int qualityFailMask;
    String contact = "WAIT";
    long ageMs;
    String raw = "";
    String userName = "";
    int fatigueThreshold = 70;
    int targetReps;
    int sessionReps;
    int totalReps;
    int walkCount;
    int squatCount;
    int deadliftCount;
    String advice = "";
    int pressMed;
    int pressLat;
    int pressDiff;
    long pressAgeMs;
    String pressStatus = "OFF";

    static LiveData fromJson(JSONObject json) {
        LiveData data = new LiveData();
        data.profileActive = json.optBoolean("profile_active", false);
        data.trainingActive = json.optBoolean("training_active", false);
        data.score = json.optInt("score", 0);
        data.action = json.optString("action", "UNKNOWN");
        data.count = json.optInt("count", 0);
        data.alert = json.optInt("alert", 0);
        data.quality = json.optInt("quality", 0);
        data.qualityLabel = json.optString("quality_label", "WAIT");
        data.qualityFailMask = json.optInt("quality_fail_mask", 0);
        data.contact = json.optString("contact", "WAIT");
        data.ageMs = json.optLong("age_ms", 0);
        data.raw = json.optString("raw", "");
        data.userName = json.optString("user_name", "");
        data.fatigueThreshold = json.optInt("fatigue_threshold", 70);
        data.targetReps = json.optInt("target_reps", 0);
        data.sessionReps = json.optInt("session_reps", 0);
        data.totalReps = json.optInt("total_reps", 0);
        data.walkCount = json.optInt("walk_count", 0);
        data.squatCount = json.optInt("squat_count", 0);
        data.deadliftCount = json.optInt("deadlift_count", 0);
        data.advice = json.optString("advice", "");
        data.pressMed = json.optInt("press_med", 0);
        data.pressLat = json.optInt("press_lat", 0);
        data.pressDiff = json.optInt("press_diff", 0);
        data.pressStatus = json.optString("press_status", "OFF");
        data.pressAgeMs = json.optLong("press_age_ms", 0);
        return data;
    }

    boolean kneeAbnormal() {
        return "MED".equals(pressStatus) || "LAT".equals(pressStatus);
    }

    boolean kneeValgus(boolean medMeansValgus) {
        return "MED".equals(pressStatus) == medMeansValgus;
    }

    String actionZh() {
        if ("SQUAT".equals(action)) return "深蹲";
        if ("DEADLIFT".equals(action)) return "硬拉";
        if ("WALK".equals(action)) return "走路";
        return "未知";
    }

    String qualityZh() {
        if ("WAIT".equals(qualityLabel)) return "等待";
        if (quality >= 80) return "良好";
        if ("OK".equals(qualityLabel)) return "合格";
        if ("WEAK".equals(qualityLabel) || "IMPROVE".equals(qualityLabel)) return "待改进";
        if ("WRONG".equals(qualityLabel)) return "错误";
        return "待改进";
    }

    String qualityReasons() {
        if ("WAIT".equals(qualityLabel)) return "等待完成动作";
        if (quality >= 80) return "发力流畅、动作标准";

        List<String> reasons = new ArrayList<>();
        if ((qualityFailMask & 1) != 0) reasons.add("动作深度不足");
        if ((qualityFailMask & 2) != 0) reasons.add("动作过快");
        if ((qualityFailMask & 4) != 0) reasons.add("节奏不均");
        if ((qualityFailMask & 8) != 0) reasons.add("肌肉激活不足");
        if ((qualityFailMask & 16) != 0) reasons.add("动作过慢");
        if ((qualityFailMask & 32) != 0) reasons.add("身体不稳定");
        if ((qualityFailMask & 64) != 0) reasons.add("压力接触丢失");
        if ((qualityFailMask & 128) != 0) reasons.add("受力不均");
        return reasons.isEmpty() ? "未发现明显问题" : android.text.TextUtils.join("、", reasons);
    }
}

final class TrainingSession {
    long id;
    String userName = "训练者";
    long startedAt;
    long endedAt;
    int reps;
    double avgFatigue;
    int peakFatigue;
    double avgQuality;
    int goodCount;
    int improveCount;
    int wrongCount;
    int squatCount;
    int deadliftCount;
    /** 训练过程中的疲劳曲线，JSON 整数数组字符串，如 "[65,70,68]" */
    String fatigueCurve = "";

    long durationSeconds() {
        return Math.max(0, (endedAt - startedAt) / 1000L);
    }

    /** 解析疲劳曲线为整型列表；无数据时返回空列表 */
    List<Integer> fatigueSeries() {
        List<Integer> result = new ArrayList<>();
        if (fatigueCurve == null || fatigueCurve.trim().isEmpty()) return result;
        try {
            JSONArray array = new JSONArray(fatigueCurve);
            for (int i = 0; i < array.length(); i++) result.add(array.optInt(i, 0));
        } catch (org.json.JSONException ignored) {
        }
        return result;
    }
}

final class SessionAccumulator {
    final long startedAt = System.currentTimeMillis();
    final String userName;
    private final List<Integer> fatigueSeries = new ArrayList<>();
    private int lastDeviceCount;
    private int reps;
    private long fatigueSum;
    private int fatigueSamples;
    private int peakFatigue;
    private long qualitySum;
    private int qualitySamples;
    private int good;
    private int improve;
    private int wrong;
    private int squat;
    private int deadlift;

    SessionAccumulator(String userName, int initialCount) {
        this.userName = userName == null || userName.isEmpty() ? "训练者" : userName;
        lastDeviceCount = initialCount;
    }

    void accept(LiveData data) {
        fatigueSum += data.score;
        fatigueSamples++;
        peakFatigue = Math.max(peakFatigue, data.score);
        fatigueSeries.add(data.score);
        if (fatigueSeries.size() > 600) {
            List<Integer> half = new ArrayList<>();
            for (int i = 0; i < fatigueSeries.size(); i += 2) half.add(fatigueSeries.get(i));
            fatigueSeries.clear();
            fatigueSeries.addAll(half);
        }
        int increment = data.sessionReps >= lastDeviceCount ? data.sessionReps - lastDeviceCount : 0;
        if (increment > 3) {
            lastDeviceCount = data.sessionReps;
            return;
        }
        increment = Math.min(increment, 20);
        if (increment > 0) {
            reps = data.sessionReps;
            if ("SQUAT".equals(data.action)) squat += increment;
            else if ("DEADLIFT".equals(data.action)) deadlift += increment;
            if (!"WAIT".equals(data.qualityLabel)) {
                qualitySum += (long) data.quality * increment;
                qualitySamples += increment;
                if (data.quality >= 80) good += increment;
                else if (data.quality < 45) wrong += increment;
                else improve += increment;
            }
        }
        lastDeviceCount = data.sessionReps;
    }

    int reps() {
        return reps;
    }

    TrainingSession finish() {
        TrainingSession session = new TrainingSession();
        session.userName = userName;
        session.startedAt = startedAt;
        session.endedAt = System.currentTimeMillis();
        session.reps = reps;
        session.avgFatigue = fatigueSamples == 0 ? 0 : (double) fatigueSum / fatigueSamples;
        session.peakFatigue = peakFatigue;
        session.avgQuality = qualitySamples == 0 ? 0 : (double) qualitySum / qualitySamples;
        session.goodCount = good;
        session.improveCount = improve;
        session.wrongCount = wrong;
        session.squatCount = squat;
        session.deadliftCount = deadlift;
        JSONArray curve = new JSONArray();
        for (int value : fatigueSeries) curve.put(value);
        session.fatigueCurve = curve.toString();
        return session;
    }
}
