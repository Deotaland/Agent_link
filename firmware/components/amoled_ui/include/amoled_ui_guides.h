#pragma once
#include "amoled_ui_language.h"

namespace amoled_ui {
struct UserGuideTopic {
    const char* title;
    const char* body;
};
inline constexpr int kUserGuideTopicCount = 5;
inline UserGuideTopic SugaratUserGuide(int topic) {
    switch (topic) {
        case 0:
            return {Tr("启动器是什么", "The Launcher"),
                    Tr("Sugarat Launcher 是 Sugarat Button 的应用主页，也支持 SDGOODS "
                       "销售的同款设备。\n\n"
                       "它管理已安装的应用，并通过下载码下载安装已审核的固件。每个应用独立运行，"
                       "返回启动器后可以选择其他应用。",
                       "Sugarat Launcher is the home screen for Sugarat Button, including units "
                       "sold by SDGOODS.\n\n"
                       "It manages installed apps and downloads reviewed firmware using a download "
                       "code. "
                       "Apps run independently. Return to Launcher to choose another app.")};
        case 1:
            return {
                Tr("连接 WiFi", "WiFi setup"),
                Tr("打开 Quick Panel > Wi-Fi。\n\n手机连接设备热点，浏览器打开 http://192.168.4.1，"
                   "提交 2.4 GHz WiFi 名称和密码。\n\n未联网时仍可使用已安装应用。"
                   "部分应用会要求单独配网，请按该应用提示操作。",
                   "Open Quick Panel > Wi-Fi. Connect your phone to the device hotspot, then open "
                   "http://192.168.4.1 in a browser. Submit your 2.4 GHz WiFi name and "
                   "password.\n\n"
                   "Installed apps still work offline. Some apps may ask for their own WiFi "
                   "setup.")};
        case 2:
            return {Tr("下载安装应用", "Install apps"),
                    Tr("1. 手机打开 https://sugarat.vercel.app/，查看应用及其六位下载码。\n"
                       "2. 左右滑到 + 卡片，点击并输入下载码。\n"
                       "3. 确认应用详情后下载安装。\n\n保持供电和联网。安装完成会重启到启动器。"
                       "空间不足时可先删除不用的应用。",
                       "1. On your phone, open:\nhttps://sugarat.vercel.app/\nFind an app and its "
                       "6-digit code.\n"
                       "2. Swipe to the + card, tap it and enter the code.\n"
                       "3. Review the app and install.\n\nKeep power and WiFi connected. "
                       "Installation restarts "
                       "into Launcher. Need space? Delete unused apps.")};
        case 3:
            return {
                Tr("应用与手势", "Apps & gestures"),
                Tr("左右滑动浏览应用与 +，点大图启动或删除。\n\n"
                   "顶部下滑：Wi-Fi、Sound、Exit、Settings；关于在 Settings > About。\n\n"
                   "左缘右滑显示 Back，松手回一级，拖回左缘取消。应用浏览页保留左右切换。\n\n"
                   "底部上滑回主页，左上返回键也可用。应用内退出先确认，启动器内只提示。",
                   "Swipe left/right to browse apps and +; tap a cover for Start/Delete.\n\n"
                   "Top down: Quick Panel (Wi-Fi, Sound, Exit, Settings). About is in Settings.\n\n"
                   "Pull right from the left edge and release: back one page. Pull back to cancel. "
                   "App browsing keeps its swipe controls.\n\n"
                   "Bottom up: home. The back arrow also works. Exit asks before leaving an app; "
                   "Launcher only shows a message.")};
        default:
            return {
                Tr("更新与帮助", "Updates & help"),
                Tr("打开 Settings > Launcher update 检查启动器更新，更新过程中保持供电。\n\n"
                   "Settings > About 显示当前应用、版本及 WiFi "
                   "状态。应用独立配网时，按应用提示操作。\n\n"
                   "Agent_link 的六位设备码用于绑定，不是应用下载码。绑定地址：\n"
                   "https://bot.deotaland.cn/#/devices",
                   "Settings > Launcher update checks for new versions. Keep power connected.\n\n"
                   "Settings > About shows the running app, version and WiFi status. Some apps "
                   "need "
                   "separate WiFi setup.\n\n"
                   "Agent_link's device code binds a device; it is different from an app "
                   "download code. "
                   "Bind at:\nhttps://bot.deotaland.cn/#/devices")};
    }
}
inline const char* DeviceBindingGuide() {
    return Tr("输入上方设备码绑定：\nhttps://bot.deotaland.cn/#/devices",
              "Enter this code to bind at:\nhttps://bot.deotaland.cn/#/devices");
}
inline const char* DownloadCodeGuide() {
    return Tr(
        "打开固件目录：\nhttps://sugarat.vercel.app/\n\n浏览可用固件，查看六位下载码。\n"
        "回到本设备输入下载码，查询详情后确认下载安装。",
        "Firmware and download codes:\nhttps://sugarat.vercel.app/\n\n"
        "Browse available firmware and find its 6-digit code.\n"
        "Enter the code here, review the app and confirm installation.");
}
}  // namespace amoled_ui
