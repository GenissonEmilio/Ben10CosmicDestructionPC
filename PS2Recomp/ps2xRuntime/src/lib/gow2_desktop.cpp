// GodOfWar2PC desktop presentation and bounded local verification hooks (GPL-3.0).
#include "runtime/desktop_video.h"
#include "rlgl.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
namespace {
int mode = 0;
bool fullscreen = true;
double started = 0, nextShot = 10;
void save() {
    std::ofstream out("gow2_settings.cfg.tmp");
    out << "fullscreen=" << int(fullscreen) << "\nscreen=" << mode << '\n'; out.close();
    std::filesystem::rename("gow2_settings.cfg.tmp", "gow2_settings.cfg");
}
}
void gow2DesktopInitialize() {
    started = GetTime();
    std::ifstream input("gow2_settings.cfg"); std::string line;
    while (std::getline(input,line)) {
        if (line.rfind("fullscreen=",0)==0) fullscreen = line.substr(11)=="1";
        if (line.rfind("screen=",0)==0) mode = std::clamp(std::atoi(line.c_str()+7),0,2);
    }
    if (!std::getenv("PS2X_HIDDEN_WINDOW")) desktop_video::setFullscreen(fullscreen);
    std::string error;
    const bool ok = desktop_video::loadShader(std::getenv("GOW2_SHADER"),error);
    if (const char *status = std::getenv("GOW2_SHADER_STATUS")) std::ofstream(status) << (ok ? "ok" : error) << '\n';
    if (!ok) std::fprintf(stderr,"[gow2-video] %s\n",error.c_str());
}
void gow2DesktopKeys() {
    if (IsKeyPressed(KEY_F11) || IsKeyPressed(KEY_F12)) {
        fullscreen = !IsWindowFullscreen(); desktop_video::setFullscreen(fullscreen); save();
    }
}
int gow2DesktopMode() { return mode; }
void gow2DesktopSnapshot() {
    const char *folder = std::getenv("GOW2_SHOT_DIR");
    const double elapsed = GetTime()-started;
    if (!folder || elapsed < nextShot) return;
    std::filesystem::create_directories(folder);
    const std::string file = std::string(folder)+"/frame_"+std::to_string(int(nextShot))+".png";
    nextShot += 20;
    rlDrawRenderBatchActive(); Image image = LoadImageFromScreen();
    ExportImage(image,file.c_str()); UnloadImage(image);
}
bool gow2DesktopShouldExit() {
    const char *seconds = std::getenv("GOW2_EXIT_SECONDS");
    return seconds && std::atof(seconds)>0 && GetTime()-started>=std::atof(seconds);
}
void gow2DesktopShutdown() { desktop_video::shutdown(); }
