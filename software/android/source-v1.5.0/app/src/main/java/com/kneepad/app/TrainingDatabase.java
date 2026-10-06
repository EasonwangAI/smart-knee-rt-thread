package com.kneepad.app;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;

import java.util.ArrayList;
import java.util.List;

final class TrainingDatabase extends SQLiteOpenHelper {
    private static final String DATABASE_NAME = "smart_knee_pad.db";
    private static final int DATABASE_VERSION = 2;

    TrainingDatabase(Context context) {
        super(context, DATABASE_NAME, null, DATABASE_VERSION);
    }

    @Override
    public void onCreate(SQLiteDatabase db) {
        db.execSQL("CREATE TABLE sessions (" +
                "id INTEGER PRIMARY KEY AUTOINCREMENT," +
                "user_name TEXT NOT NULL," +
                "started_at INTEGER NOT NULL," +
                "ended_at INTEGER NOT NULL," +
                "reps INTEGER NOT NULL," +
                "avg_fatigue REAL NOT NULL," +
                "peak_fatigue INTEGER NOT NULL," +
                "avg_quality REAL NOT NULL," +
                "good_count INTEGER NOT NULL," +
                "improve_count INTEGER NOT NULL," +
                "wrong_count INTEGER NOT NULL," +
                "squat_count INTEGER NOT NULL," +
                "deadlift_count INTEGER NOT NULL," +
                "fatigue_curve TEXT DEFAULT '')");
    }

    @Override
    public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
        if (oldVersion < 2) {
            db.execSQL("ALTER TABLE sessions ADD COLUMN fatigue_curve TEXT DEFAULT ''");
        }
    }

    long insert(TrainingSession session) {
        ContentValues values = new ContentValues();
        values.put("user_name", session.userName);
        values.put("started_at", session.startedAt);
        values.put("ended_at", session.endedAt);
        values.put("reps", session.reps);
        values.put("avg_fatigue", session.avgFatigue);
        values.put("peak_fatigue", session.peakFatigue);
        values.put("avg_quality", session.avgQuality);
        values.put("good_count", session.goodCount);
        values.put("improve_count", session.improveCount);
        values.put("wrong_count", session.wrongCount);
        values.put("squat_count", session.squatCount);
        values.put("deadlift_count", session.deadliftCount);
        values.put("fatigue_curve", session.fatigueCurve == null ? "" : session.fatigueCurve);
        return getWritableDatabase().insert("sessions", null, values);
    }

    List<TrainingSession> allSessions() {
        List<TrainingSession> result = new ArrayList<>();
        try (Cursor cursor = getReadableDatabase().query("sessions", null, null, null, null, null, "started_at DESC")) {
            while (cursor.moveToNext()) result.add(read(cursor));
        }
        return result;
    }

    TrainingSession find(long id) {
        try (Cursor cursor = getReadableDatabase().query("sessions", null, "id=?", new String[]{String.valueOf(id)}, null, null, null)) {
            return cursor.moveToFirst() ? read(cursor) : null;
        }
    }

    private TrainingSession read(Cursor cursor) {
        TrainingSession item = new TrainingSession();
        item.id = cursor.getLong(cursor.getColumnIndexOrThrow("id"));
        item.userName = cursor.getString(cursor.getColumnIndexOrThrow("user_name"));
        item.startedAt = cursor.getLong(cursor.getColumnIndexOrThrow("started_at"));
        item.endedAt = cursor.getLong(cursor.getColumnIndexOrThrow("ended_at"));
        item.reps = cursor.getInt(cursor.getColumnIndexOrThrow("reps"));
        item.avgFatigue = cursor.getDouble(cursor.getColumnIndexOrThrow("avg_fatigue"));
        item.peakFatigue = cursor.getInt(cursor.getColumnIndexOrThrow("peak_fatigue"));
        item.avgQuality = cursor.getDouble(cursor.getColumnIndexOrThrow("avg_quality"));
        item.goodCount = cursor.getInt(cursor.getColumnIndexOrThrow("good_count"));
        item.improveCount = cursor.getInt(cursor.getColumnIndexOrThrow("improve_count"));
        item.wrongCount = cursor.getInt(cursor.getColumnIndexOrThrow("wrong_count"));
        item.squatCount = cursor.getInt(cursor.getColumnIndexOrThrow("squat_count"));
        item.deadliftCount = cursor.getInt(cursor.getColumnIndexOrThrow("deadlift_count"));
        int curveIndex = cursor.getColumnIndex("fatigue_curve");
        item.fatigueCurve = curveIndex >= 0 ? cursor.getString(curveIndex) : "";
        return item;
    }
}
