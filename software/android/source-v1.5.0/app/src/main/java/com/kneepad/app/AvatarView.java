package com.kneepad.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

/**
 * 圆形头像视图：有图片时圆形裁切显示图片，无图片时显示「首字母 + 背景色」。
 */
final class AvatarView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private Bitmap bitmap;
    private String label = "";
    private int bgColor = Color.rgb(40, 103, 232);

    AvatarView(Context context) {
        super(context);
    }

    void setAvatar(Bitmap bmp) {
        bitmap = bmp;
        invalidate();
    }

    void setLabel(String text, int color) {
        label = text == null ? "" : text;
        bgColor = color;
        bitmap = null;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float cx = getWidth() / 2f;
        float cy = getHeight() / 2f;
        float radius = Math.min(getWidth(), getHeight()) / 2f;
        if (radius <= 0) return;

        Path clip = new Path();
        clip.addCircle(cx, cy, radius, Path.Direction.CW);
        canvas.save();
        canvas.clipPath(clip);
        if (bitmap != null) {
            RectF dst = new RectF(cx - radius, cy - radius, cx + radius, cy + radius);
            canvas.drawBitmap(bitmap, null, dst, paint);
        } else {
            paint.setColor(bgColor);
            canvas.drawCircle(cx, cy, radius, paint);
            paint.setColor(Color.WHITE);
            paint.setTextSize(radius * 0.9f);
            paint.setTextAlign(Paint.Align.CENTER);
            Paint.FontMetrics fm = paint.getFontMetrics();
            float baseline = cy - (fm.ascent + fm.descent) / 2f;
            canvas.drawText(label, cx, baseline, paint);
            paint.setTextAlign(Paint.Align.LEFT);
        }
        canvas.restore();
    }
}
