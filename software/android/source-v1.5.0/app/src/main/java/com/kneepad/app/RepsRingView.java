package com.kneepad.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

/**
 * 分项计数环图：仅渲染三段式圆环（环心显示总计）。
 * 图例由 MainActivity 作为兄弟元素单独构建，避免在 View 内重叠。
 */
final class RepsRingView extends View {
    static final int WALK_COLOR = Color.rgb(22, 136, 91);    // 绿
    static final int SQUAT_COLOR = Color.rgb(40, 103, 232);  // 蓝
    static final int DEADLIFT_COLOR = Color.rgb(211, 120, 22); // 橙
    private static final int MUTED = Color.rgb(107, 118, 144);
    private static final int TRACK = Color.rgb(232, 237, 245);
    private static final int INK = Color.rgb(23, 32, 51);

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int walk;
    private int squat;
    private int deadlift;
    private boolean hasData;

    RepsRingView(Context context) {
        super(context);
    }

    void setCounts(int walk, int squat, int deadlift) {
        this.walk = Math.max(0, walk);
        this.squat = Math.max(0, squat);
        this.deadlift = Math.max(0, deadlift);
        hasData = (walk + squat + deadlift) > 0;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float width = getWidth();
        float height = getHeight();
        float cx = width / 2f;
        float cy = height / 2f;

        float strokeWidth = dp(14);
        float maxRadius = Math.min(width / 2f - strokeWidth / 2f - dp(6), height / 2f - strokeWidth / 2f - dp(6));
        float radius = maxRadius;
        if (radius < dp(28)) radius = dp(28);
        RectF ring = new RectF(cx - radius, cy - radius, cx + radius, cy + radius);

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(strokeWidth);
        paint.setStrokeCap(Paint.Cap.BUTT);

        int total = walk + squat + deadlift;
        if (!hasData) {
            paint.setColor(TRACK);
            canvas.drawArc(ring, 0, 360, false, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setTextSize(dp(13));
            paint.setColor(MUTED);
            String hint = "完成动作后显示分布";
            canvas.drawText(hint, cx - paint.measureText(hint) / 2f, cy + dp(5), paint);
        } else {
            float start = -90f;
            start = drawSegment(canvas, ring, start, walk, total, WALK_COLOR);
            start = drawSegment(canvas, ring, start, squat, total, SQUAT_COLOR);
            drawSegment(canvas, ring, start, deadlift, total, DEADLIFT_COLOR);

            paint.setStyle(Paint.Style.FILL);
            paint.setTextSize(dp(22));
            paint.setColor(INK);
            paint.setTextAlign(Paint.Align.CENTER);
            canvas.drawText(String.valueOf(total), cx, cy + dp(1), paint);
            paint.setTextSize(dp(10));
            paint.setColor(MUTED);
            canvas.drawText("总动作", cx, cy + dp(18), paint);
            paint.setTextAlign(Paint.Align.LEFT);
        }
    }

    private float drawSegment(Canvas canvas, RectF ring, float start, int value, int total, int color) {
        if (value <= 0) return start;
        float sweep = 360f * value / total;
        paint.setColor(color);
        canvas.drawArc(ring, start, sweep, false, paint);
        return start + sweep;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
