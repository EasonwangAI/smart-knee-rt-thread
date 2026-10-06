package com.kneepad.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

/**
 * 压力对称性仪表：将内侧(med)/外侧(lat)压力值量化为两个横条与百分比，
 * 中央显示偏差(diff)。偏差超过阈值时整体变红提示"受力不均"，
 * 是膝盖内扣/外翻最直观的量化展示。
 */
final class PressureGaugeView extends View {
    private static final int BLUE = Color.rgb(40, 103, 232);
    private static final int RED = Color.rgb(220, 60, 77);
    private static final int GREEN = Color.rgb(22, 136, 91);
    private static final int MUTED = Color.rgb(107, 118, 144);
    private static final int TRACK = Color.rgb(232, 237, 245);
    private static final int INK = Color.rgb(23, 32, 51);

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int med;
    private int lat;
    private int diff;
    private String status = "OFF";
    private boolean hasData;

    PressureGaugeView(Context context) {
        super(context);
        setMinimumHeight(dp(96));
    }

    void setValues(int med, int lat, int diff, String status, boolean hasData) {
        this.med = Math.max(0, med);
        this.lat = Math.max(0, lat);
        this.diff = diff;
        this.status = status == null ? "OFF" : status;
        this.hasData = hasData;
        invalidate();
    }

    private String statusText() {
        if (!hasData) return "等待压力数据";
        if ("BAL".equals(status)) return "受力均衡";
        if ("MED".equals(status)) return "内侧偏高 · 提示内扣";
        if ("LAT".equals(status)) return "外侧偏高 · 提示外翻";
        if ("CAL".equals(status)) return "校准中，请站稳 2 秒";
        if ("ERR".equals(status)) return "传感器异常";
        return "压力状态 " + status;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float density = getResources().getDisplayMetrics().density;
        float width = getWidth();
        int total = med + lat;
        float medPct = total <= 0 ? 0 : med * 100f / total;
        float latPct = total <= 0 ? 0 : lat * 100f / total;
        boolean unbalanced = hasData && Math.abs(diff) >= 20;

        // 行 1：内侧压力
        float labelLeft = dp(6), labelRight = width - dp(6);
        float y1 = dp(8);
        paint.setTextSize(dp(11));
        paint.setColor(MUTED);
        canvas.drawText("内侧", labelLeft, y1 + dp(11), paint);
        canvas.drawText("外侧", labelRight - paint.measureText("外侧"), y1 + dp(11), paint);
        paint.setColor(unbalanced ? RED : INK);
        String pct = total <= 0 ? "--" : String.format(java.util.Locale.CHINA, "%.0f%%", medPct);
        canvas.drawText(pct, labelRight - paint.measureText(pct) - dp(46), y1 + dp(11), paint);

        // 行 2：压力条（内侧 0->50% 宽，外侧 50%->100% 宽，形成对称镜像）
        float barTop = y1 + dp(15);
        float barHeight = dp(16);
        float halfWidth = width / 2f - dp(4);
        RectF medTrack = new RectF(dp(4), barTop, halfWidth, barTop + barHeight);
        RectF medBar = new RectF(medTrack.left + dp(2), barTop + dp(2),
                medTrack.left + dp(2) + (medTrack.width() - dp(4)) * Math.min(1f, medPct / 100f),
                barTop + barHeight - dp(2));
        RectF latTrack = new RectF(halfWidth + dp(4), barTop, width - dp(4), barTop + barHeight);
        RectF latBar = new RectF(latTrack.right - dp(2) - (latTrack.width() - dp(4)) * Math.min(1f, latPct / 100f),
                barTop + dp(2), latTrack.right - dp(2), barTop + barHeight - dp(2));

        paint.setColor(TRACK);
        canvas.drawRoundRect(medTrack, dp(8), dp(8), paint);
        canvas.drawRoundRect(latTrack, dp(8), dp(8), paint);
        paint.setColor(unbalanced ? RED : BLUE);
        if (total > 0) {
            canvas.drawRoundRect(medBar, dp(6), dp(6), paint);
            canvas.drawRoundRect(latBar, dp(6), dp(6), paint);
        }

        // 行 3：偏差与状态
        float y3 = barTop + barHeight + dp(10);
        paint.setTextSize(dp(11));
        if (!hasData) {
            paint.setColor(MUTED);
            canvas.drawText("等待压力数据", width / 2f - paint.measureText("等待压力数据") / 2f, y3 + dp(11), paint);
            return;
        }
        paint.setColor(unbalanced ? RED : GREEN);
        String diffText = String.format(java.util.Locale.CHINA, "偏差 %d", diff);
        canvas.drawText(diffText, width / 2f - paint.measureText(diffText) / 2f, y3 + dp(11), paint);
        paint.setColor(MUTED);
        String statusText = statusText();
        canvas.drawText(statusText, width / 2f - paint.measureText(statusText) / 2f, y3 + dp(24), paint);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
