package com.kneepad.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.text.TextUtils;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.List;

/**
 * 简易文件提供器：用于把导出目录（cacheDir/exports）中的 CSV 文件
 * 通过 content:// URI 分享给微信/文件管理器等应用。
 * 项目未使用 androidx，因此自行实现 FileProvider 的最小等价物。
 * 注意：ContentProvider 必须为 public 类并提供 public 无参构造器，
 * 否则系统无法反射实例化，App 启动时直接崩溃。
 */
public final class CsvFileProvider extends ContentProvider {
    static final String AUTHORITY = "com.kneepad.app.fileprovider";

    public CsvFileProvider() {
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    static Uri uriFor(File file) {
        return Uri.parse("content://" + AUTHORITY + "/csv/" + Uri.encode(file.getName()));
    }

    @Override
    public String getType(Uri uri) {
        return "text/csv";
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        List<String> segments = uri.getPathSegments();
        if (segments.size() < 2) throw new FileNotFoundException("无效路径");
        String name = Uri.decode(segments.get(1));
        if (TextUtils.isEmpty(name) || name.contains("/") || name.contains("..")) {
            throw new FileNotFoundException("无效文件名");
        }
        File dir = new File(getContext().getCacheDir(), "exports");
        File file = new File(dir, name);
        if (!file.exists()) throw new FileNotFoundException(name);
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection,
                                  String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
