#include "Editor.h"
#include "State.h"
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

namespace beatweaver {
namespace {
using namespace Gdiplus;
constexpr wchar_t kWindowClass[] = L"BeatWeaver.NativeEditor.1";
constexpr float kPi = 3.14159265358979323846f;
const Color background(255, 18, 21, 26), panel(255, 25, 29, 35), border(255, 44, 50, 60);
const Color textColor(255, 220, 227, 235), muted(255, 124, 139, 155);
const Color teal(255, 85, 222, 197), amber(255, 246, 186, 93);
RECT rc(int x, int y, int w, int h) { return RECT{x, y, x + w, y + h}; }
bool contains(RECT r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
std::wstring wide(const char* s) {
    if (!s || !*s) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &result[0], count);
    result.pop_back();
    return result;
}
std::string narrow(const wchar_t* s) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &result[0], count, nullptr, nullptr);
    result.pop_back();
    return result;
}
void fill(Graphics& g, RECT r, Color c) {
    SolidBrush brush(c); g.FillRectangle(&brush, int(r.left), int(r.top), int(r.right-r.left), int(r.bottom-r.top));
}
void line(Graphics& g, float x1, float y1, float x2, float y2, Color c, float width=1.0f) {
    Pen pen(c,width); g.DrawLine(&pen,x1,y1,x2,y2);
}
void label(Graphics& g, const std::wstring& s, RECT r, float size, Color c,
           bool bold=false, StringAlignment align=StringAlignmentNear) {
    FontFamily family(L"Segoe UI");
    Font font(&family, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush brush(c); StringFormat format;
    format.SetAlignment(align); format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter); format.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(s.c_str(), static_cast<INT>(s.size()), &font,
        RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top), static_cast<REAL>(r.right-r.left), static_cast<REAL>(r.bottom-r.top)), &format, &brush);
}
enum Action {
    SlotTime=100, SlotVolume=200, TabTime=300, TabVolume, Copy, Paste, Reset, Undo,
    Snap, Lock, Slide, CurveMenu, Presets, Rename, Menu, ToggleBase=400, ParamBase=500
};
struct Hit { RECT rect; int id; std::wstring hint; };
}

struct Editor::Impl {
    Model& model;
    AutomationCallback callback;
    void* context;
    HWND hwnd=nullptr, renameEdit=nullptr;
    HINSTANCE instance=nullptr;
    ULONG_PTR gdiplusToken=0;
    bool volume=false, locked=false, slide=false, changing=false;
    bool copied=false, renameVolume=false;
    int selectedPoint=-1, hover=-1, dragParameter=-1, renameSlot=0;
    int snap=16, pointDrag=-1, originMouseY=0;
    float originParameter=0;
    RECT graph=rc(360,190,714,288);
    std::vector<Hit> hits;
    std::wstring hint=L"Double-click the graph to add a point. Drag to shape. Right-click for curve options.";
    Envelope clipboard{}, dragOrigin{};
    struct History { bool volume; int slot; Envelope envelope; };
    std::vector<History> history;
    WNDPROC oldEditProc=nullptr;

    Impl(Model& m, AutomationCallback cb, void* ctx): model(m),callback(cb),context(ctx) {
        GdiplusStartupInput input; GdiplusStartup(&gdiplusToken,&input,nullptr);
    }
    ~Impl() { close(); if(instance)UnregisterClassW(kWindowClass,instance); if(gdiplusToken) GdiplusShutdown(gdiplusToken); }
    int slot(bool v) const { return parameterSlot(model.getParam(v ? Param::VolumeSlot : Param::TimeSlot)); }
    Envelope envelope() const { return model.getEnvelope(volume,slot(volume)); }
    Color accent() const { return volume ? amber : teal; }
    void invalidate() { if(hwnd) InvalidateRect(hwnd,nullptr,FALSE); }
    void set(Param p,float value) {
        value=clamp01(value); model.setParam(p,value);
        if(callback) callback(context,static_cast<int>(p),value);
        invalidate();
    }
    void remember() {
        history.push_back({volume,slot(volume),envelope()});
        if(history.size()>64) history.erase(history.begin());
    }
    void save(const Envelope& e) { model.setEnvelope(volume,slot(volume),e); invalidate(); }
    void select(bool v,int index) {
        volume=v; selectedPoint=-1;
        set(v ? Param::VolumeSlot : Param::TimeSlot,slotParameter(index));
        if(model.getParam(Param::Link)>.5f) set(v ? Param::TimeSlot : Param::VolumeSlot,slotParameter(index));
    }
    void hit(RECT r,int id,const wchar_t* help) { hits.push_back({r,id,help}); }
    int at(int x,int y) const {
        for(auto it=hits.rbegin();it!=hits.rend();++it) if(contains(it->rect,x,y)) return it->id;
        return -1;
    }
    void button(Graphics& g,RECT r,const std::wstring& title,int id,bool active=false,const wchar_t* help=L"") {
        Color c=active?accent():muted;
        fill(g,r,active?Color(255,40,58,59):(hover==id?Color(255,46,52,61):Color(255,31,36,44)));
        Pen pen(active?Color(255,67,112,106):border); g.DrawRectangle(&pen,int(r.left),int(r.top),int(r.right-r.left-1),int(r.bottom-r.top-1));
        label(g,title,r,11,c,true,StringAlignmentCenter); hit(r,id,help);
    }
    void bank(Graphics& g,bool v) {
        int y=v?374:113; Color ac=v?amber:teal;
        label(g,v?L"VOLUME BANK":L"TIME BANK",rc(33,y,185,25),11,ac,true);
        label(g,L"36 SLOTS",rc(214,y,83,25),9,muted,false,StringAlignmentFar);
        for(int i=0;i<kSlotCount;++i) {
            const int x=33+(i%6)*44, sy=y+38+(i/6)*29;
            RECT r=rc(x,sy,39,24); bool active=slot(v)==i;
            fill(g,r,active?Color(255,v?79:35,v?64:72,v?42:68):Color(255,35,40,48));
            if(active) { Pen p(ac);g.DrawRectangle(&p,x+.5f,sy+.5f,38.0f,23.0f); }
            else if(hover==(v?SlotVolume:SlotTime)+i) { Pen p(muted);g.DrawRectangle(&p,x+.5f,sy+.5f,38.0f,23.0f); }
            wchar_t n[8];swprintf_s(n,L"%02d",i+1);
            label(g,n,r,11,active?ac:Color(255,151,164,180),active,StringAlignmentCenter);
            const auto e=model.getEnvelope(v,i);
            std::wstring help=wide(e.name)+L"  |  Click to select. Right-click to rename, copy or move.";
            hits.push_back({r,(v?SlotVolume:SlotTime)+i,help});
        }
        const auto e=model.getEnvelope(v,slot(v));
        label(g,wide(e.name),rc(34,y+218,257,27),12,volume==v?textColor:muted);
        line(g,33,float(y+253),292,float(y+253),border);
    }
    PointF screen(const Point& p) const {
        return PointF(graph.left+p.x*(graph.right-graph.left),
            graph.top+(volume?1-p.y:p.y)*(graph.bottom-graph.top));
    }
    Point graphPoint(int x,int y) const {
        Point result;
        result.x=clamp01(float(x-graph.left)/float(graph.right-graph.left));
        float yy=clamp01(float(y-graph.top)/float(graph.bottom-graph.top)); result.y=volume?1-yy:yy;
        if(snap>0) { result.x=std::round(result.x*snap)/snap;
            if(GetKeyState(VK_SHIFT)>=0) result.y=std::round(result.y*16)/16; }
        return result;
    }
    int nearPoint(int x,int y) const {
        const auto e=envelope(); int result=-1; float nearest=100;
        for(int i=0;i<e.count;++i) { auto p=screen(e.points[i]);
            float distance=(p.X-x)*(p.X-x)+(p.Y-y)*(p.Y-y);
            if(distance<nearest){nearest=distance;result=i;} }
        return result;
    }
    void drawGraph(Graphics& g) {
        const Envelope e=envelope(); const Color ac=accent();
        fill(g,graph,Color(255,20,24,30));
        for(int i=0;i<=32;++i) {
            float x=graph.left+i*float(graph.right-graph.left)/32;
            line(g,x,float(graph.top),x,float(graph.bottom),i%8==0?Color(255,58,67,78):(i%2==0?Color(255,36,43,53):Color(255,28,34,42)));
        }
        for(int i=0;i<=8;++i) {
            float y=graph.top+i*float(graph.bottom-graph.top)/8;
            line(g,float(graph.left),y,float(graph.right),y,i%2==0?Color(255,48,56,67):Color(255,33,40,49));
        }
        std::vector<PointF> samples;
        for(int i=0;i<=714;++i) {
            float x=float(i)/714;
            Point p;p.x=x;p.y=evaluateEnvelope(e,x);samples.push_back(screen(p));
        }
        std::vector<PointF> area=samples;
        area.push_back(PointF(float(graph.right),float(graph.bottom)));
        area.push_back(PointF(float(graph.left),float(graph.bottom)));
        SolidBrush wash(Color(23,ac.GetR(),ac.GetG(),ac.GetB()));g.FillPolygon(&wash,area.data(),static_cast<INT>(area.size()));
        Pen curve(ac,2.0f);g.DrawLines(&curve,samples.data(),static_cast<INT>(samples.size()));
        float playX=graph.left+clamp01(model.playhead.load(std::memory_order_relaxed))*(graph.right-graph.left);
        line(g,playX,float(graph.top),playX,float(graph.bottom),Color(130,235,243,249),1.0f);
        for(int i=0;i<e.count;++i) {
            auto p=screen(e.points[i]); float radius=i==selectedPoint?5.0f:3.5f;
            SolidBrush b(i==selectedPoint?textColor:ac);g.FillEllipse(&b,p.X-radius,p.Y-radius,radius*2,radius*2);
            Pen ring(background,1.5f);g.DrawEllipse(&ring,p.X-radius,p.Y-radius,radius*2,radius*2);
        }
        for(int i=0;i<=4;++i) {
            wchar_t t[20];
            if(volume) swprintf_s(t,L"%d",100-i*25);else swprintf_s(t,L"%.1fx",i*.5f);
            label(g,t,rc(329,graph.top+i*(graph.bottom-graph.top)/4-8,26,16),9,muted,false,StringAlignmentFar);
        }
        for(int i=0;i<4;++i) {
            wchar_t t[8];swprintf_s(t,L"%d",i+1);
            label(g,t,rc(graph.left+i*(graph.right-graph.left)/4+7,graph.bottom+4,60,18),10,muted);
        }
        label(g,volume?L"LEVEL %":L"DELAY / PATTERN LENGTH",rc(360,168,210,17),9,muted,true);
        label(g,L"CYCLE POSITION",rc(902,graph.bottom+4,171,18),9,muted,false,StringAlignmentFar);
        if(locked) label(g,L"EDIT LOCKED",rc(590,300,280,40),17,Color(200,175,187,199),true,StringAlignmentCenter);
    }
    void knob(Graphics& g,int x,int y,Param parameter,const wchar_t* caption) {
        float value=model.getParam(parameter); int id=ParamBase+static_cast<int>(parameter);
        RECT r=rc(x-35,y-6,70,108); hit(r,id,L"Drag up or down to adjust. Shift = fine. Double-click = default. Mouse wheel adjusts.");
        const Color ac=parameter==Param::VolumeMix?amber:teal;
        Pen track(Color(255,48,57,68),4);g.DrawArc(&track,float(x-22),float(y+8),44.0f,44.0f,135.0f,270.0f);
        Pen active(ac,4);g.DrawArc(&active,float(x-22),float(y+8),44.0f,44.0f,135.0f,270.0f*value);
        SolidBrush face(Color(255,34,41,50));g.FillEllipse(&face,x-17,y+13,34,34);
        float a=(135.0f+270.0f*value)*kPi/180.0f;
        line(g,float(x)+std::cos(a)*8,float(y+30)+std::sin(a)*8,float(x)+std::cos(a)*14,float(y+30)+std::sin(a)*14,textColor,2);
        label(g,caption,rc(x-43,y+60,86,18),9,muted,true,StringAlignmentCenter);
        char out[48]{};parameterText(parameter,value,out,48);
        label(g,wide(out),rc(x-43,y+80,86,18),12,textColor,false,StringAlignmentCenter);
    }
    void toggle(Graphics& g,RECT r,Param p,const wchar_t* caption,const wchar_t* help) {
        button(g,r,caption,ToggleBase+static_cast<int>(p),model.getParam(p)>.5f,help);
    }
    void choice(Graphics& g,RECT r,Param p,const wchar_t* caption,const wchar_t* help) {
        char t[48]{};parameterText(p,model.getParam(p),t,48);
        button(g,r,std::wstring(caption)+L"  "+wide(t)+L"  \x25BE",ParamBase+static_cast<int>(p),false,help);
    }
    void paint(HDC dc) {
        Bitmap bitmap(Editor::width(),Editor::height(),PixelFormat32bppPARGB);Graphics g(&bitmap);
        g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        hits.clear();fill(g,rc(0,0,1120,740),background);
        fill(g,rc(0,0,1120,80),Color(255,23,27,33));line(g,0,79,1120,79,border);
        // An original signal glyph: offset steps folding into one continuous path.
        Pen glyph(teal,3);PointF logo[]={{26,49},{34,49},{34,31},{43,31},{43,48},{51,48},{51,36},{59,36}};
        g.DrawLines(&glyph,logo,8);
        label(g,L"BEATWEAVER",rc(73,18,320,33),26,textColor,true);
        label(g,L"TIME + VOLUME SHAPING",rc(75,49,320,16),9,muted,true);
        label(g,L"CREATE MOTION. RESHAPE THE GROOVE.",rc(438,15,388,25),10,muted,false,StringAlignmentCenter);
        label(g,L"IN",rc(483,45,26,16),8,muted);label(g,L"OUT",rc(659,45,29,16),8,muted);
        fill(g,rc(513,51,122,4),border);fill(g,rc(693,51,122,4),border);
        fill(g,rc(513,51,int(clamp01(model.inputPeak.load(std::memory_order_relaxed))*122),4),teal);
        fill(g,rc(693,51,int(clamp01(model.outputPeak.load(std::memory_order_relaxed))*122),4),amber);
        toggle(g,rc(928,25,96,29),Param::Bypass,L"BYPASS",L"Pass input audio through without the effect.");
        button(g,rc(1035,25,60,29),L"MENU",Menu,false,L"Bank tools and editor help.");
        fill(g,rc(20,96,289,592),panel);bank(g,false);bank(g,true);
        label(g,L"MIDI NOTES",rc(34,648,100,18),9,muted,true);
        label(g,L"12\x2013" L"47 VOL   /   48\x2013" L"83 TIME",rc(34,668,255,17),9,muted);
        fill(g,rc(326,96,774,438),panel);
        button(g,rc(344,111,110,28),L"TIME CURVE",TabTime,!volume,L"Edit the active time envelope.");
        button(g,rc(461,111,110,28),L"VOLUME CURVE",TabVolume,volume,L"Edit the active volume envelope.");
        const auto e=envelope();
        label(g,wide(e.name),rc(588,112,251,26),13,textColor,true);
        button(g,rc(842,112,50,25),L"COPY",Copy,false,L"Copy the current envelope.");
        button(g,rc(898,112,50,25),L"PASTE",Paste,false,L"Paste the copied envelope into this slot.");
        button(g,rc(954,112,54,25),L"RESET",Reset,false,L"Reset this envelope to a neutral shape.");
        button(g,rc(1014,112,65,25),L"UNDO",Undo,false,L"Undo the last envelope change. Ctrl+Z.");
        drawGraph(g);
        std::wstring snapText=snap?L"SNAP  1/"+std::to_wstring(snap):L"SNAP  FREE";
        button(g,rc(345,505,104,22),snapText,Snap,snap>0,L"Set horizontal grid snapping. Hold Shift while dragging for free vertical movement.");
        button(g,rc(457,505,88,22),L"EDIT LOCK",Lock,locked,L"Prevent accidental envelope edits.");
        button(g,rc(553,505,67,22),L"SLIDE",Slide,slide,L"Move later points together when dragging an inner point.");
        button(g,rc(628,505,105,22),L"CURVE  \x25BE",CurveMenu,false,L"Choose the selected point's outgoing segment curve.");
        button(g,rc(971,505,108,22),L"PRESETS  \x25BE",Presets,false,L"Load one of the 36 original patterns into the current slot.");
        if(selectedPoint>=0&&selectedPoint<e.count) {
            wchar_t info[80];swprintf_s(info,L"P%d   X %.3f   Y %.3f",selectedPoint+1,e.points[selectedPoint].x,e.points[selectedPoint].y);
            label(g,info,rc(742,505,220,22),10,muted);
        }
        fill(g,rc(326,548,774,140),panel);
        knob(g,371,559,Param::TimeMix,L"TIME MIX");knob(g,450,559,Param::VolumeMix,L"VOL MIX");
        knob(g,529,559,Param::Attack,L"ATTACK");knob(g,608,559,Param::Release,L"RELEASE");
        knob(g,687,559,Param::Tension,L"TENSION");knob(g,766,559,Param::Scratch,L"SCRATCH");knob(g,845,559,Param::Output,L"OUTPUT");
        choice(g,rc(885,562,194,26),Param::Length,L"LENGTH",L"Duration of one complete envelope cycle in beats.");
        choice(g,rc(885,596,194,26),Param::TriggerSync,L"TRIGGER",L"Quantization for selecting and retriggering slots.");
        choice(g,rc(885,630,194,26),Param::PositionSync,L"POSITION",L"Position synchronization grid; free preserves trigger phase.");
        toggle(g,rc(20,699,72,24),Param::Link,L"LINK",L"Select the same slot number in both banks.");
        toggle(g,rc(99,699,72,24),Param::MidiEnable,L"MIDI",L"Enable MIDI note selection: notes 12-47 volume; notes 48-83 time.");
        toggle(g,rc(178,699,83,24),Param::OneShot,L"ONE SHOT",L"Play one envelope cycle per trigger, then return to the neutral slot.");
        toggle(g,rc(268,699,69,24),Param::Hold,L"HOLD",L"Freeze the last captured bar or capture the next bar, according to Hold Mode.");
        toggle(g,rc(344,699,86,24),Param::KeyHeld,L"KEY HELD",L"Keep a selected MIDI slot active while its trigger key is held.");
        choice(g,rc(437,699,145,24),Param::Smooth,L"CLICK FADE",L"Crossfade duration for time jumps. Mouse wheel adjusts the duration.");
        toggle(g,rc(589,699,72,24),Param::RemoveDC,L"DC CUT",L"Remove DC offset from the processed audio.");
        choice(g,rc(668,699,153,24),Param::HoldMode,L"HOLD MODE",L"Off, repeat the last captured bar, or capture the next bar for repeat.");
        label(g,L"64-BIT  /  STEREO",rc(930,701,163,20),9,muted,false,StringAlignmentFar);
        label(g,hint,rc(21,725,1074,14),9,muted);
        Graphics out(dc);out.DrawImage(&bitmap,0,0);
    }
    void updateHint(int id) {
        std::wstring next=L"Double-click: add point  |  Right-click: curve / delete  |  Mouse wheel: segment tension  |  Ctrl+Z: undo";
        for(const auto& h:hits) if(h.id==id&&!h.hint.empty()) { next=h.hint;break; }
        if(next!=hint) { hint=next;
            // The native tooltip provides full hints without consuming graph space.
            if(hwnd) SetWindowTextW(hwnd,hint.c_str()); }
    }
    void menuAt(HMENU menu,int x,int y) {
        POINT p{x,y};ClientToScreen(hwnd,&p);int command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr);
        DestroyMenu(menu);if(command) commandMenu(command);
    }
    void popup(int action,int x,int y) {
        HMENU menu=CreatePopupMenu();
        if(action==Snap) {
            const int options[]={0,4,8,16,32,64};
            for(int i=0;i<6;++i) { std::wstring s=options[i]?L"1/"+std::to_wstring(options[i]):L"Free";
                AppendMenuW(menu,MF_STRING|(snap==options[i]?MF_CHECKED:0),1000+i,s.c_str()); }
        } else if(action==Presets) {
            for(int i=0;i<36;++i) {const auto e=makePreset(volume,i);std::wstring s=std::to_wstring(i+1)+L"  "+wide(e.name);AppendMenuW(menu,MF_STRING,1100+i,s.c_str());}
        } else if(action==CurveMenu) {
            const wchar_t* names[]={L"Linear",L"Hold / step",L"Smooth",L"Single curve (wheel to bend)",L"Double curve",L"Alternate single curve",L"Alternate double curve",L"Stairs",L"Smooth stairs",L"Pulse",L"Wave",L"Half sine"};
            const auto e=envelope();int curve=selectedPoint>=0&&selectedPoint<e.count?e.points[selectedPoint].curve:-1;
            for(int i=0;i<12;++i) AppendMenuW(menu,MF_STRING|(curve==i?MF_CHECKED:0),1200+i,names[i]);
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,1250,L"Delete point");
        } else if(action==Menu) {
            AppendMenuW(menu,MF_STRING,1306,L"Open bank (.bwv)...");AppendMenuW(menu,MF_STRING,1307,L"Save bank as (.bwv)...");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING,1300,L"Rename current slot...");AppendMenuW(menu,MF_STRING,1301,L"Copy current envelope");
            AppendMenuW(menu,MF_STRING|(copied?0:MF_GRAYED),1302,L"Paste envelope");
            AppendMenuW(menu,MF_STRING,1303,L"Reset current envelope");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING,1304,L"Restore all factory patterns...");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING,1305,L"Mouse and keyboard help");
        }
        menuAt(menu,x,y);
    }
    void parameterMenu(Param p,int x,int y) {
        HMENU menu=CreatePopupMenu();int count=0;
        if(p==Param::Length) count=7;else if(p==Param::HoldMode) count=3;else count=9;
        for(int i=0;i<count;++i) {float v=float(i)/float(count-1);char t[48]{};parameterText(p,v,t,48);
            AppendMenuW(menu,MF_STRING|(std::abs(model.getParam(p)-v)<.04f?MF_CHECKED:0),1400+i,wide(t).c_str()); }
        POINT pt{x,y};ClientToScreen(hwnd,&pt);int result=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,nullptr);
        DestroyMenu(menu);if(result>=1400&&result<1400+count)set(p,float(result-1400)/(count-1));
    }
    void commandMenu(int c) {
        if(c>=1000&&c<1006) { const int options[]={0,4,8,16,32,64};snap=options[c-1000]; }
        else if(c>=1100&&c<1136&&!locked) { remember();save(makePreset(volume,c-1100));selectedPoint=-1; }
        else if(c>=1200&&c<1212&&!locked) {
            auto e=envelope();if(selectedPoint<0&&e.count)selectedPoint=0;
            if(selectedPoint>=0&&selectedPoint<e.count) { remember();e.points[selectedPoint].curve=c-1200;save(e); }
        } else if(c==1250) deletePoint();
        else if(c==1300) beginRename(volume,slot(volume));
        else if(c==1301) perform(Copy,0,0);else if(c==1302)perform(Paste,0,0);else if(c==1303)perform(Reset,0,0);
        else if(c==1306||c==1307)bankFile(c==1307);
        else if(c==1304) {
            if(MessageBoxW(hwnd,L"Replace all 72 envelopes with the original factory patterns?",L"Restore bank",MB_YESNO|MB_ICONQUESTION)==IDYES){model.resetPresets();history.clear();}
        } else if(c==1305) {
            MessageBoxW(hwnd,L"ENVELOPES\nDouble-click graph: add a point\nDrag a point: move it (endpoints keep their position)\nShift: free vertical movement / fine knob adjustment\nAlt+drag a segment or use mouse wheel: bend tension\nRight-click a point: choose curve or delete\nDelete: remove selected inner point\nCtrl+C / Ctrl+V: copy / paste envelope\nCtrl+Z: undo envelope edits\n\nSLOTS\nLeft-click: select and trigger\nRight-click: rename, copy, paste or swap\nDouble-click: rename\n\nPARAMETERS\nDrag a knob up/down, or use the mouse wheel\nDouble-click a knob: restore default\n\nMIDI\nNotes 12-47 select volume slots; 48-83 select time slots.\nThe host stores all curves and settings in the plugin state.",L"BeatWeaver controls",MB_OK|MB_ICONINFORMATION);
        }
        invalidate();
    }
    void bankFile(bool writing) {
        wchar_t filename[32768]=L"BeatWeaver.bwv";
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=GetAncestor(hwnd,GA_ROOT);
        dialog.lpstrFilter=L"BeatWeaver bank (*.bwv)\0*.bwv\0All files (*.*)\0*.*\0\0";
        dialog.lpstrFile=filename;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"bwv";
        dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|(writing?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
        if(!(writing?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog)))return;
        bool ok=false;
        if(writing) {
            const auto data=encodeState(model);
            HANDLE file=CreateFileW(filename,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE){DWORD bytes=0;ok=WriteFile(file,data.data(),static_cast<DWORD>(data.size()),&bytes,nullptr)&&bytes==data.size();CloseHandle(file);}
        } else {
            HANDLE file=CreateFileW(filename,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE){LARGE_INTEGER size{};
                if(GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<=1024*1024){std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));DWORD bytes=0;
                    if(ReadFile(file,data.data(),static_cast<DWORD>(data.size()),&bytes,nullptr)&&bytes==data.size())ok=decodeState(model,data.data(),data.size());}
                CloseHandle(file);
            }
            if(ok){history.clear();selectedPoint=-1;if(callback)for(int i=0;i<kParameterCount;++i)callback(context,i,model.getParam(static_cast<Param>(i)));}
        }
        if(!ok)MessageBoxW(hwnd,writing?L"The bank could not be saved. Choose a writable location.":L"This file could not be loaded as a valid BeatWeaver bank.",L"BeatWeaver bank",MB_OK|MB_ICONERROR);
        invalidate();
    }
    void perform(int id,int x,int y) {
        if(id>=SlotTime&&id<SlotTime+36)select(false,id-SlotTime);
        else if(id>=SlotVolume&&id<SlotVolume+36)select(true,id-SlotVolume);
        else if(id==TabTime) {volume=false;selectedPoint=-1;}else if(id==TabVolume){volume=true;selectedPoint=-1;}
        else if(id==Copy){clipboard=envelope();copied=true;}
        else if(id==Paste&&copied&&!locked){remember();save(clipboard);selectedPoint=-1;}
        else if(id==Reset&&!locked){remember();auto e=defaultEnvelope(volume);strncpy_s(e.name,"Neutral",_TRUNCATE);save(e);selectedPoint=-1;}
        else if(id==Undo&&!history.empty()&&!locked){auto item=history.back();history.pop_back();model.setEnvelope(item.volume,item.slot,item.envelope);volume=item.volume;select(volume,item.slot);}
        else if(id==Lock)locked=!locked;else if(id==Slide)slide=!slide;
        else if(id==Snap||id==Presets||id==CurveMenu||id==Menu)popup(id,x,y);
        else if(id>=ToggleBase&&id<ToggleBase+kParameterCount){auto p=static_cast<Param>(id-ToggleBase);set(p,model.getParam(p)>.5f?0.0f:1.0f);}
        else if(id>=ParamBase&&id<ParamBase+kParameterCount) {
            auto p=static_cast<Param>(id-ParamBase);
            if(p==Param::Length||p==Param::TriggerSync||p==Param::PositionSync||p==Param::HoldMode||p==Param::Smooth)parameterMenu(p,x,y);
            else {dragParameter=static_cast<int>(p);originParameter=model.getParam(p);originMouseY=y;SetCapture(hwnd);}
        }
        invalidate();
    }
    void deletePoint() {
        if(locked)return;auto e=envelope();if(selectedPoint<=0||selectedPoint>=e.count-1)return;
        remember();for(int i=selectedPoint;i<e.count-1;++i)e.points[i]=e.points[i+1];--e.count;selectedPoint=-1;save(e);
    }
    void leftDown(int x,int y) {
        SetFocus(hwnd);
        if(contains(graph,x,y)&&!locked) {
            selectedPoint=nearPoint(x,y);pointDrag=selectedPoint;
            if(selectedPoint>=0){remember();dragOrigin=envelope();SetCapture(hwnd);}
            else if(GetKeyState(VK_MENU)<0) {
                auto p=graphPoint(x,y);auto e=envelope();
                for(int i=0;i<e.count-1;++i)if(p.x>=e.points[i].x&&p.x<=e.points[i+1].x){selectedPoint=i;pointDrag=-2;originMouseY=y;dragOrigin=e;remember();SetCapture(hwnd);break;}
            }
            invalidate();return;
        }
        perform(at(x,y),x,y);
    }
    void mouseMove(int x,int y) {
        if(dragParameter>=0) {
            float scale=GetKeyState(VK_SHIFT)<0?.001f:.005f;
            set(static_cast<Param>(dragParameter),originParameter+(originMouseY-y)*scale);return;
        }
        if(pointDrag==-2&&!locked) {
            auto e=dragOrigin;if(e.points[selectedPoint].curve<3)e.points[selectedPoint].curve=static_cast<int>(Curve::Tension);
            e.points[selectedPoint].tension=std::clamp(dragOrigin.points[selectedPoint].tension+(originMouseY-y)*.012f,-1.0f,1.0f);save(e);return;
        }
        if(pointDrag>=0&&!locked) {
            auto e=dragOrigin;auto p=graphPoint(x,y);const int n=pointDrag;
            if(n==0||n==e.count-1)p.x=e.points[n].x;
            else {
                float lower=e.points[n-1].x+.001f,upper=e.points[n+1].x-.001f;
                p.x=lower<=upper?std::clamp(p.x,lower,upper):std::clamp(p.x,e.points[n-1].x,e.points[n+1].x);
            }
            if(slide&&n>0&&n<e.count-1) {
                float dx=p.x-e.points[n].x,dy=p.y-e.points[n].y;
                dx=std::min(dx,1.0f-e.points[e.count-2].x-.001f);
                for(int i=n;i<e.count;++i){if(i<e.count-1)e.points[i].x=clamp01(e.points[i].x+dx);e.points[i].y=clamp01(e.points[i].y+dy);}
            } else {e.points[n].x=p.x;e.points[n].y=p.y;}
            save(e);return;
        }
        int next=at(x,y);if(next!=hover){hover=next;updateHint(next);invalidate();}
        SetCursor(LoadCursor(nullptr,contains(graph,x,y)&&!locked?IDC_CROSS:IDC_ARROW));
    }
    void doubleClick(int x,int y) {
        int id=at(x,y);
        if(id>=SlotTime&&id<SlotTime+36){beginRename(false,id-SlotTime);return;}
        if(id>=SlotVolume&&id<SlotVolume+36){beginRename(true,id-SlotVolume);return;}
        if(id>=ParamBase&&id<ParamBase+kParameterCount) {auto p=static_cast<Param>(id-ParamBase);set(p,defaultParameter(p));return;}
        if(!contains(graph,x,y)||locked)return;
        auto e=envelope();if(e.count>=kMaxPoints||nearPoint(x,y)>=0)return;
        auto p=graphPoint(x,y);int index=1;while(index<e.count&&e.points[index].x<p.x)++index;
        if(index>=e.count||p.x-e.points[index-1].x<.001f||e.points[index].x-p.x<.001f)return;
        remember();for(int i=e.count;i>index;--i)e.points[i]=e.points[i-1];e.points[index]=p;++e.count;selectedPoint=index;save(e);
    }
    void rightClick(int x,int y) {
        int id=at(x,y);
        if((id>=SlotTime&&id<SlotTime+36)||(id>=SlotVolume&&id<SlotVolume+36)) {
            bool v=id>=SlotVolume;int index=id-(v?SlotVolume:SlotTime);select(v,index);
            HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1300,L"Rename slot...");
            AppendMenuW(menu,MF_STRING,1301,L"Copy envelope");AppendMenuW(menu,MF_STRING|(copied?0:MF_GRAYED),1302,L"Paste envelope");
            AppendMenuW(menu,MF_STRING,1303,L"Reset envelope");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuW(menu,MF_STRING|(index>0?0:MF_GRAYED),1500,L"Swap with previous slot");
            AppendMenuW(menu,MF_STRING|(index<35?0:MF_GRAYED),1501,L"Swap with next slot");
            POINT p{x,y};ClientToScreen(hwnd,&p);int c=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr);DestroyMenu(menu);
            if(c==1500||c==1501) {int other=index+(c==1500?-1:1);auto a=envelope(),b=model.getEnvelope(v,other);model.setEnvelope(v,index,b);model.setEnvelope(v,other,a);select(v,other);}
            else if(c)commandMenu(c);
        } else if(contains(graph,x,y)) {
            selectedPoint=nearPoint(x,y);
            if(selectedPoint<0){const auto e=envelope();const float px=graphPoint(x,y).x;
                for(int i=0;i<e.count-1;++i)if(px>=e.points[i].x&&px<=e.points[i+1].x){selectedPoint=i;break;}}
            popup(CurveMenu,x,y);
        }invalidate();
    }
    void wheel(int x,int y,int delta) {
        int id=at(x,y);
        if(id>=ParamBase&&id<ParamBase+kParameterCount) {auto p=static_cast<Param>(id-ParamBase);set(p,model.getParam(p)+(delta>0?1:-1)*(GetKeyState(VK_SHIFT)<0?.001f:.02f));}
        else if(contains(graph,x,y)&&!locked) {
            auto e=envelope();auto p=graphPoint(x,y);int index=0;
            for(int i=0;i<e.count-1;++i)if(p.x>=e.points[i].x&&p.x<=e.points[i+1].x){index=i;break;}
            remember();if(e.points[index].curve<3)e.points[index].curve=static_cast<int>(Curve::Tension);e.points[index].tension=std::clamp(e.points[index].tension+(delta>0?.08f:-.08f),-1.0f,1.0f);selectedPoint=index;save(e);
        }
    }
    static LRESULT CALLBACK editProc(HWND edit,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(edit,GWLP_USERDATA));
        if(self&&(msg==WM_KEYDOWN)&&(wp==VK_RETURN||wp==VK_ESCAPE)){self->finishRename(wp==VK_RETURN);return 0;}
        return self?CallWindowProcW(self->oldEditProc,edit,msg,wp,lp):DefWindowProcW(edit,msg,wp,lp);
    }
    void beginRename(bool v,int index) {
        if(renameEdit)finishRename(true);renameVolume=v;renameSlot=index;
        const auto e=model.getEnvelope(v,index);volume=v;
        renameEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",wide(e.name).c_str(),WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            585,111,250,29,hwnd,reinterpret_cast<HMENU>(4000),instance,nullptr);
        if(renameEdit){SendMessageW(renameEdit,EM_SETLIMITTEXT,24,0);SetWindowLongPtrW(renameEdit,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
            oldEditProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(renameEdit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(editProc)));
            SendMessageW(renameEdit,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);SetFocus(renameEdit);SendMessageW(renameEdit,EM_SETSEL,0,-1);}
        invalidate();
    }
    void finishRename(bool saveName) {
        if(!renameEdit)return;HWND edit=renameEdit;renameEdit=nullptr;
        if(saveName) {wchar_t name[64]{};GetWindowTextW(edit,name,64);auto text=narrow(name);if(!text.empty()){
            auto e=model.getEnvelope(renameVolume,renameSlot);history.push_back({renameVolume,renameSlot,e});
            std::memset(e.name,0,sizeof(e.name));strncpy_s(e.name,text.c_str(),_TRUNCATE);model.setEnvelope(renameVolume,renameSlot,e);}}
        DestroyWindow(edit);SetFocus(hwnd);invalidate();
    }
    static LRESULT CALLBACK wndProc(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
        Impl* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(msg==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->hwnd=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,msg,wp,lp);
        auto mouseX=[&](){return static_cast<int>(static_cast<short>(LOWORD(lp)));};
        auto mouseY=[&](){return static_cast<int>(static_cast<short>(HIWORD(lp)));};
        switch(msg) {
        case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(window,&ps);self->paint(dc);EndPaint(window,&ps);return 0;}
        case WM_PRINTCLIENT:case WM_PRINT:if(wp)self->paint(reinterpret_cast<HDC>(wp));return 0;
        case WM_ERASEBKGND:return 1;
        case WM_TIMER:self->invalidate();return 0;
        case WM_LBUTTONDOWN:self->leftDown(mouseX(),mouseY());return 0;
        case WM_LBUTTONUP:self->dragParameter=-1;self->pointDrag=-1;ReleaseCapture();return 0;
        case WM_CAPTURECHANGED:self->dragParameter=-1;self->pointDrag=-1;return 0;
        case WM_MOUSEMOVE:self->mouseMove(mouseX(),mouseY());return 0;
        case WM_LBUTTONDBLCLK:self->doubleClick(mouseX(),mouseY());return 0;
        case WM_RBUTTONUP:self->rightClick(mouseX(),mouseY());return 0;
        case WM_MOUSEWHEEL:{POINT p{mouseX(),mouseY()};ScreenToClient(window,&p);self->wheel(p.x,p.y,GET_WHEEL_DELTA_WPARAM(wp));return 0;}
        case WM_COMMAND:if(LOWORD(wp)==4000&&HIWORD(wp)==EN_KILLFOCUS)self->finishRename(true);return 0;
        case WM_KEYDOWN:
            if(wp==VK_DELETE||wp==VK_BACK)self->deletePoint();
            else if(GetKeyState(VK_CONTROL)<0&&wp=='C')self->perform(Copy,0,0);
            else if(GetKeyState(VK_CONTROL)<0&&wp=='V')self->perform(Paste,0,0);
            else if(GetKeyState(VK_CONTROL)<0&&wp=='Z')self->perform(Undo,0,0);
            else if(wp==VK_F2)self->beginRename(self->volume,self->slot(self->volume));
            return 0;
        case WM_DESTROY:KillTimer(window,1);self->renameEdit=nullptr;self->hwnd=nullptr;return 0;
        }
        return DefWindowProcW(window,msg,wp,lp);
    }
    bool open(HWND parent,HINSTANCE module) {
        if(hwnd)return true;instance=module;
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.style=CS_DBLCLKS;wc.lpfnWndProc=wndProc;wc.hInstance=module;
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.lpszClassName=kWindowClass;
        RegisterClassExW(&wc);
        hwnd=CreateWindowExW(0,kWindowClass,L"BeatWeaver",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN|WS_TABSTOP,0,0,Editor::width(),Editor::height(),parent,nullptr,module,this);
        if(hwnd){SetTimer(hwnd,1,50,nullptr);UpdateWindow(hwnd);return true;}return false;
    }
    void close() {if(renameEdit)finishRename(true);if(hwnd){DestroyWindow(hwnd);hwnd=nullptr;}}
};

Editor::Editor(Model& model,AutomationCallback callback,void* context):impl_(std::make_unique<Impl>(model,callback,context)){}
Editor::~Editor()=default;
bool Editor::open(HWND parent,HINSTANCE instance){return impl_->open(parent,instance);}
void Editor::close(){impl_->close();}
HWND Editor::window()const noexcept{return impl_->hwnd;}

} // namespace beatweaver
