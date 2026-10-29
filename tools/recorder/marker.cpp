// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "marker.h"
#include "marker_internal.h"
#include <obs.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>

namespace xen::recorder {
namespace {
constexpr char kMarkerSourceId[]="xen_recorder_sync_marker";
constexpr char kBackgroundSourceId[]="xen_recorder_synthetic_background";
std::int64_t steady_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::int64_t system_ns() noexcept {
    FILETIME value{}; GetSystemTimePreciseAsFileTime(&value);
    ULARGE_INTEGER combined{}; combined.LowPart=value.dwLowDateTime; combined.HighPart=value.dwHighDateTime;
    return static_cast<std::int64_t>((combined.QuadPart-116444736000000000ULL)*100);
}
struct GraphicsScope {
    GraphicsScope() { obs_enter_graphics(); }
    ~GraphicsScope() { obs_leave_graphics(); }
};
struct MarkerData {
    std::mutex mutex;
    detail::MarkerLifecycle lifecycle;
    detail::RenderQueue events;
    gs_texture_t* texture=nullptr;
    std::uint32_t width=560,height=72;
    bool rendered=false;
};
struct BackgroundData {
    gs_texture_t* texture=nullptr;
    std::uint32_t width=0,height=0;
};

// 只在mark冷路径分配GDI对象。纹理每个像素都不透明，文字不依赖桌面覆盖窗口。
std::vector<std::uint32_t> raster_marker(const std::string& id,std::uint32_t width,std::uint32_t height) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width)*height);
    const std::wstring text(id.begin(),id.end()); // 已在冷路径严格限制为安全ASCII。
    const auto dc=CreateCompatibleDC(nullptr);
    if (!dc) return {};
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=static_cast<LONG>(width); info.bmiHeader.biHeight=-static_cast<LONG>(height);
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    const auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); DeleteDC(dc); return {}; }
    const auto old_bitmap=SelectObject(dc,bitmap);
    RECT client{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    const auto brush=CreateSolidBrush(RGB(18,66,120));
    bool success=brush && FillRect(dc,&client,brush)!=0;
    if (brush) DeleteObject(brush);
    int font_height=std::min(34,std::max(8,static_cast<int>(height)-24));
    HFONT font=nullptr; HGDIOBJ old_font=nullptr;
    SIZE extent{};
    for (;;) {
        font=CreateFontW(-font_height,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,FIXED_PITCH,L"Consolas");
        if (!font) { success=false; break; }
        old_font=SelectObject(dc,font);
        if (!GetTextExtentPoint32W(dc,text.data(),static_cast<int>(text.size()),&extent)) { success=false; break; }
        if (extent.cx<=static_cast<LONG>(width)-24 && extent.cy<=static_cast<LONG>(height)-12) break;
        SelectObject(dc,old_font); old_font=nullptr; DeleteObject(font); font=nullptr;
        if (--font_height<8) { success=false; break; }
    }
    if (success && font) {
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
        const int text_y=std::max(0,(static_cast<int>(height)-static_cast<int>(extent.cy))/2);
        success=TextOutW(dc,12,text_y,text.data(),static_cast<int>(text.size()))!=FALSE;
    }
    success=success && GdiFlush()!=FALSE;
    if (success) {
        std::memcpy(pixels.data(),bits,pixels.size()*sizeof(std::uint32_t));
        for (auto& pixel:pixels) pixel|=0xff000000U;
    }
    if (old_font) SelectObject(dc,old_font);
    if (font) DeleteObject(font);
    SelectObject(dc,old_bitmap); DeleteObject(bitmap); DeleteDC(dc);
    return success ? std::move(pixels) : std::vector<std::uint32_t>{};
}
bool draw_texture(gs_texture_t* texture,std::uint32_t width,std::uint32_t height) noexcept {
    const auto effect=obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if (!effect || !texture) return false;
    const auto image=gs_effect_get_param_by_name(effect,"image");
    if (!image) return false;
    gs_effect_set_texture(image,texture);
    bool drawn=false;
    while (gs_effect_loop(effect,"Draw")) { gs_draw_sprite(texture,0,width,height); drawn=true; }
    return drawn;
}
const char* marker_name(void*) { return "Xen录像同步标记"; }
const char* background_name(void*) { return "Xen录制合成背景"; }
void* marker_create(obs_data_t* settings,obs_source_t*) {
    const auto width=obs_data_get_int(settings,"width"),height=obs_data_get_int(settings,"height");
    if (width<64 || width>4096 || height<24 || height>1024) return nullptr;
    auto* result=new(std::nothrow) MarkerData;
    if (result) { result->width=static_cast<std::uint32_t>(width); result->height=static_cast<std::uint32_t>(height); }
    return result;
}
void marker_destroy(void* opaque) {
    auto* data=static_cast<MarkerData*>(opaque);
    if (!data) return;
    // 与渲染同样按graphics→状态锁顺序；绝不持状态锁等待图形线程。
    { GraphicsScope graphics; if (data->texture) gs_texture_destroy(data->texture); }
    delete data;
}
std::uint32_t marker_width(void* opaque) { return static_cast<MarkerData*>(opaque)->width; }
std::uint32_t marker_height(void* opaque) { return static_cast<MarkerData*>(opaque)->height; }
void marker_render(void* opaque,gs_effect_t*) {
    auto& data=*static_cast<MarkerData*>(opaque);
    const auto began=steady_ns(),system_began=system_ns();
    const auto frame_time=obs_get_video_frame_time();
    std::lock_guard lock(data.mutex);
    const auto state=data.lifecycle.frame(began,frame_time);
    const bool drawn=state.draw && data.texture && draw_texture(data.texture,data.width,data.height);
    if ((state.record && drawn) || state.timed_out) {
        detail::RawRenderedMarker event;
        event.id=data.lifecycle.id(); event.first=!data.rendered && drawn; event.timed_out=state.timed_out;
        if (drawn) data.rendered=true;
        event.begin_steady=began; event.begin_system=system_began;
        event.end_system=system_ns(); event.end_steady=steady_ns();
        event.frame_time=state.timed_out ? 0 : frame_time;
        data.events.push(event);
    }
}
void* background_create(obs_data_t* settings,obs_source_t*) {
    const auto width=obs_data_get_int(settings,"width"),height=obs_data_get_int(settings,"height");
    if (width<1 || width>8192 || height<1 || height>8192) return nullptr;
    auto* result=new(std::nothrow) BackgroundData;
    if (!result) return nullptr;
    result->width=static_cast<std::uint32_t>(width); result->height=static_cast<std::uint32_t>(height);
    const std::uint32_t pixel=0xff242a34U;
    const std::uint8_t* bytes=reinterpret_cast<const std::uint8_t*>(&pixel);
    { GraphicsScope graphics; result->texture=gs_texture_create(1,1,GS_BGRA,1,&bytes,0); }
    if (!result->texture) { delete result; return nullptr; }
    return result;
}
void background_destroy(void* opaque) {
    auto* data=static_cast<BackgroundData*>(opaque);
    if (!data) return;
    { GraphicsScope graphics; if (data->texture) gs_texture_destroy(data->texture); }
    delete data;
}
std::uint32_t background_width(void* opaque) { return static_cast<BackgroundData*>(opaque)->width; }
std::uint32_t background_height(void* opaque) { return static_cast<BackgroundData*>(opaque)->height; }
void background_render(void* opaque,gs_effect_t*) {
    auto& data=*static_cast<BackgroundData*>(opaque); draw_texture(data.texture,data.width,data.height);
}
bool source_registered(const char* wanted) noexcept {
    const char* id=nullptr;
    for (std::size_t index=0;obs_enum_input_types(index,&id);++index)
        if (id && std::strcmp(id,wanted)==0) return true;
    return false;
}
void register_sources() {
    // 注册属于当前libobs实例；同进程shutdown/startup后重新检查，而非进程静态一次标志。
    if (!source_registered(kMarkerSourceId)) {
        obs_source_info info{}; info.id=kMarkerSourceId; info.type=OBS_SOURCE_TYPE_INPUT;
        info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
        info.get_name=marker_name; info.create=marker_create; info.destroy=marker_destroy;
        info.get_width=marker_width; info.get_height=marker_height; info.video_render=marker_render;
        obs_register_source(&info);
    }
    if (!source_registered(kBackgroundSourceId)) {
        obs_source_info info{}; info.id=kBackgroundSourceId; info.type=OBS_SOURCE_TYPE_INPUT;
        info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
        info.get_name=background_name; info.create=background_create; info.destroy=background_destroy;
        info.get_width=background_width; info.get_height=background_height; info.video_render=background_render;
        obs_register_source(&info);
    }
}
}

MarkerSource::~MarkerSource() { reset(); }
bool MarkerSource::create(std::uint32_t width,std::uint32_t height) noexcept {
    try {
        if (source_ || width<64 || width>4096 || height<24 || height>1024) return false;
        register_sources();
        auto* settings=obs_data_create();
        if (!settings) return false;
        obs_data_set_int(settings,"width",width); obs_data_set_int(settings,"height",height);
        source_=obs_source_create_private(kMarkerSourceId,"Xen录像同步标记",settings);
        obs_data_release(settings);
        if (source_ && !obs_obj_get_data(source_)) { reset(); return false; }
        return source_!=nullptr;
    } catch (...) { return false; }
}
bool MarkerSource::mark(const std::string& id,std::uint32_t duration_ms) noexcept {
    try {
        if (!source_ || !detail::valid_marker_id(id) || duration_ms<1 || duration_ms>10000) return false;
        auto& data=*static_cast<MarkerData*>(obs_obj_get_data(source_));
        { std::lock_guard lock(data.mutex); if (data.lifecycle.busy()) return false; }
        const auto pixels=raster_marker(id,data.width,data.height);
        if (pixels.empty()) return false;
        const std::uint8_t* bytes=reinterpret_cast<const std::uint8_t*>(pixels.data());
        // 先进入graphics，后锁状态。生成像素和GDI调用不持渲染状态锁。
        GraphicsScope graphics;
        auto* texture=gs_texture_create(data.width,data.height,GS_BGRA,1,&bytes,0);
        if (!texture) return false;
        std::lock_guard lock(data.mutex);
        if (!data.lifecycle.request(id,steady_ns(),duration_ms)) { gs_texture_destroy(texture); return false; }
        const auto old=data.texture; data.texture=texture; data.rendered=false;
        if (old) gs_texture_destroy(old);
        return true;
    } catch (...) { return false; }
}
MarkerDrain MarkerSource::drain() {
    MarkerDrain result;
    if (!source_) return result;
    auto& data=*static_cast<MarkerData*>(obs_obj_get_data(source_));
    std::array<detail::RawRenderedMarker,detail::kRenderQueueCapacity> events{};
    std::size_t count=0;
    {
        std::lock_guard lock(data.mutex);
        const auto now=steady_ns();
        if (data.lifecycle.frame(now,0).timed_out) {
            detail::RawRenderedMarker event; event.id=data.lifecycle.id(); event.timed_out=true;
            event.begin_steady=event.end_steady=now; event.begin_system=event.end_system=system_ns();
            data.events.push(event);
        }
        while (count<events.size() && data.events.pop(events[count])) ++count;
        result.dropped=data.events.dropped();
    }
    result.markers.reserve(count);
    for (std::size_t index=0;index<count;++index) {
        const auto& raw=events[index]; RenderedMarker event;
        event.id=raw.id.data(); event.first_render=raw.first;
        event.render_begin_steady_ns=raw.begin_steady; event.render_end_steady_ns=raw.end_steady;
        event.render_begin_system_unix_ns=raw.begin_system; event.render_end_system_unix_ns=raw.end_system;
        event.render_frame_time_ns=raw.frame_time;
        if (raw.timed_out) event.error="标记请求后两秒内没有取得实际渲染帧";
        result.markers.push_back(std::move(event));
    }
    return result;
}
void MarkerSource::reset() noexcept {
    if (source_) { obs_source_release(source_); source_=nullptr; }
}
obs_source_t* create_synthetic_background(std::uint32_t width,std::uint32_t height) noexcept {
    try {
        if (width<1 || width>8192 || height<1 || height>8192) return nullptr;
        register_sources();
        auto* settings=obs_data_create();
        if (!settings) return nullptr;
        obs_data_set_int(settings,"width",width); obs_data_set_int(settings,"height",height);
        auto* result=obs_source_create_private(kBackgroundSourceId,"Xen合成不透明背景",settings);
        obs_data_release(settings);
        if (result && !obs_obj_get_data(result)) { obs_source_release(result); return nullptr; }
        return result;
    } catch (...) { return nullptr; }
}
}
