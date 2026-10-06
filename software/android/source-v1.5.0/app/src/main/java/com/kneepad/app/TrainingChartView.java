package com.kneepad.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class TrainingChartView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private List<TrainingSession> sessions = Collections.emptyList();

    TrainingChartView(Context context) {
        super(context);
        setMinimumHeight(dp(210));
        setBackgroundColor(Color.WHITE);
    }

    void setSessions(List<TrainingSession> values) {
        sessions = values == null ? Collections.emptyList() : new ArrayList<>(values);
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float left = dp(38), top = dp(22), right = getWidth() - dp(16), bottom = getHeight() - dp(34);
        paint.setStrokeWidth(dp(1));
        paint.setColor(Color.rgb(222, 229, 239));
        canvas.drawLine(left, bottom, right, bottom, paint);
        canvas.drawLine(left, top, left, bottom, paint);
        paint.setTextSize(dp(11));
        paint.setColor(Color.rgb(107, 118, 144));
        if (sessions.isEmpty()) {
            canvas.drawText("完成训练后将在这里显示近 7 次趋势", left + dp(12), top + dp(40), paint);
            return;
        }
        int count = Math.min(7, sessions.size());
        float slot = (right - left) / count;
        float maxReps = 1;
        for (int i = 0; i < count; i++) maxReps = Math.max(maxReps, sessions.get(i).reps);
        for (int i = 0; i < count; i++) {
            TrainingSession session = sessions.get(count - 1 - i);
            float height = (bottom - top) * 0.78f * session.reps / maxReps;
            float x = left + i * slot + slot * 0.22f;
            paint.setColor(Color.rgb(40, 103, 232));
            canvas.drawRoundRect(x, bottom - height, x + slot * 0.56f, bottom, dp(4), dp(4), paint);
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setColor(Color.rgb(23, 32, 51));
            canvas.drawText(String.valueOf(session.reps), x + slot * 0.28f, bottom - height - dp(6), paint);
            paint.setColor(Color.rgb(107, 118, 144));
            canvas.drawText((i + 1) + "次", x + slot * 0.28f, bottom + dp(18), paint);
        }
        paint.setTextAlign(Paint.Align.LEFT);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
