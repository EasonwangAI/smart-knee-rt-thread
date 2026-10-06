# 开源整理说明

2026-10-06 更新在原仓库目录基础上加入本次提供的 STM32 工程、ESP32 v1.3.2、Android v1.2.4 安装包及归档中的 Android v1.5.0 源码，保留原有训练工具和数据。来源哈希见 [VERSIONS.md](VERSIONS.md)。

## 公开内容

- STM32：原创应用、板级驱动、工程配置、RT-Thread 内核、HAL/CMSIS 和链接脚本。
- ESP32：原样保留指定的 v1.3.2 Arduino 源码，采用规范工程文件夹名。
- Android：原 APK 仅去掉 `.1` 下载后缀；开发源码按实际 v1.5.0 单独存放。
- 数据：保留运行匹配特征、CSV 和 SQLite；移除公开副本中的直接受试者姓名与本机绝对路径。
- 文档：首页、版本、接线/复现、协议、实际演示门控与文件清单。

## 未上传内容

`.workbuddy` 的 JDK/SDK/工具链、Gradle 缓存、Debug/BuildV 编译目录、历史 APK 堆积、签名私钥、个人运行日志和原始大压缩包未纳入此次提交。指定 APK 与一份归档内固件位于单独的下载目录。

## 许可证

项目继续使用 Apache-2.0，根目录 LICENSE 补全为标准全文。原项目贡献者版权保留在 [NOTICE](../NOTICE)。

- RT-Thread：保留 `firmware/rt-thread-studio-project/rt-thread/LICENSE` 与原文件版权。
- STM32 HAL/CMSIS：保留各组件已有 LICENSE 与原始版权声明，不能把全部第三方源码归为本团队原创。
- Android/ESP32：保留提供的原始源码；没有为本次发布生成新的签名密钥。
- 数据：外部样本的来源、再分发授权与实验标签仍应由数据提供者维护，项目代码许可证不自动授予第三方数据权利。

`docs/source-notes` 中是原工程留下的历史说明，可能存在过期阈值或版本描述；当前入口文档及本次源码优先。
