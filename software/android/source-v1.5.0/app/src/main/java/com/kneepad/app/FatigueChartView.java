package com.kneepad.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.view.View;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * 实时疲劳曲线：滚动折线图，展示疲劳评分(0-100)随时间的变化，
 * 支持训练中实时追加、结束后用历史数据回放。
 */
final class FatigueChartView extends View {
    private static final int BLUE = Color.rgb(40, 103, 232);
    private static final int RED = Color.rgb(220, 60, 77);
    private static final int MUTED = Color.rgb(107, 118, 144);
    private static final int GRID = Color.rgb(228, 233, 242);
    private static final int INK = Color.rgb(23, 32, 51);

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final List<Float> points = new ArrayList<>();
    private int threshold = 70;
    private int maxPoints = 180;
    private String hint = "训练开始后显示疲劳趋势";

    FatigueChartView(Context context) {
        super(context);
        setMinimumHeight(dp(150));
        setBackgroundColor(Color.WHITE);
    }

    void reset() {
        points.clear();
        invalidate();
    }

    void setThreshold(int value) {
        threshold = Math.max(1, Math.min(100, value));
        invalidate();
    }

    void setHint(String value) {
        hint = value == null ? "" : value;
        invalidate();
    }

    void addPoint(float value) {
        points.add(Math.max(0f, Math.min(100f, value)));
        if (points.size() > maxPoints) points.remove(0);
        invalidate();
    }

    void setSeries(List<Integer> values) {
        points.clear();
        if (values != null) {
            for (int value : values) points.add((float) Math.max(0, Math.min(100, value)));
        }
        invalidate();
    }

    List<Float> points() {
        return Collections.unmodifiableList(points);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float left = dp(30), right = getWidth() - dp(8);
        float top = dp(10), bottom = getHeight() - dp(26);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(1));
        paint.setColor(GRID);
        paint.setTextSize(dp(10));
        paint.setTextAlign(Paint.Align.RIGHT);
        paint.setColor(MUTED);
        for (int i = 0; i <= 4; i++) {
            float y = top + (bottom - top) * i / 4f;
            canvas.drawLine(left, y, right, y, paint);
            paint.setColor(GRID);
            canvas.drawLine(left, y, right, y, paint);
            paint.setColor(MUTED);
            canvas.drawText(String.valueOf(100 - i * 25), left - dp(4), y + dp(3), paint);
        }
        paint.setTextAlign(Paint.Align.LEFT);

        // 疲劳提醒线
        float thresholdY = bottom - (bottom - top) * threshold / 100f;
        paint.setColor(RED);
        paint.setStrokeWidth(dp(1.5f));
        paint.setPathEffect(new android.graphics.DashPathEffect(new float[]{dp(6), dp(5)}, 0));
        canvas.drawLine(left, thresholdY, right, thresholdY, paint);
        paint.setPathEffect(null);
        paint.setTextSize(dp(10));
        canvas.drawText("提醒线 " + threshold, left + dp(4), thresholdY - dp(4), paint);

        if (points.isEmpty()) {
            paint.setColor(MUTED);
            canvas.drawText(hint, left + dp(4), top + dp(22), paint);
            return;
        }

        int n = points.size();
        float slot = (right - left) / Math.max(1, n - 1);
        Path line = new Path();
        for (int i = 0; i < n; i++) {
            float x = left + i * slot;
            float y = bottom - (bottom - top) * points.get(i) / 100f;
            if (i == 0) line.moveTo(x, y);
            else line.lineTo(x, y);
        }
        paint.setColor(BLUE);
        paint.setStrokeWidth(dp(2));
        canvas.drawPath(line, paint);

        // 最新点
        float lastX = left + (n - 1) * slot;
        float lastY = bottom - (bottom - top) * points.get(n - 1) / 100f;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(points.get(n - 1) >= threshold ? RED : BLUE);
        canvas.drawCircle(lastX, lastY, dp(3.5f), paint);

        paint.setTextSize(dp(11));
        paint.setColor(INK);
        canvas.drawText(String.valueOf(Math.round(points.get(n - 1))), lastX - dp(6), lastY - dp(8), paint);

        // 填充渐变
        Path fill = new Path();
        fill.moveTo(left + 0f, bottom);
        for (int i = 0; i < n; i++) {
            float x = left + i * slot;
            float y = bottom - (bottom - top) * points.get(i) / 100f;
            fill.lineTo(x, y);
        }
        fill.lineTo(left + (n - 1) * slot, bottom);
        fill.close();
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new LinearGradient(0, top, 0, bottom,
                Color.argb(70, 40, 103, 232), Color.argb(0, 40, 103, 232), Shader.TileMode.CLAMP));
        canvas.drawPath(fill, paint);
        paint.setShader(null);
        paint.setStyle(Paint.Style.STROKE);

        paint.setStyle(Paint.Style.FILL);
        paint.setTextSize(dp(10));
        paint.setColor(MUTED);
        canvas.drawText("最近 " + n + " 点", left + dp(4), bottom + dp(16), paint);
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
