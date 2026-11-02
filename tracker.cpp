#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <memory>
#include <cstdio>
#include <algorithm>
#include <rex/ppc/context.h>
#include "wml.h"
#include "wml_pagequery.h"
#include "collectibles.h"

namespace {
const WmlApi* api;const WmlMod* self;
WmlGuestFunction original_map,original_batch,original_minibatch,original_pos,original_color,original_bitmap,original_input,original_update,original_action,original_pan;
std::vector<tracker::Location> locations,missing;
std::atomic<bool> enabled{false};std::atomic<unsigned> mode{3};
std::atomic<uint64_t> map_seen{0};
unsigned cds=0,tags=0;bool cds_ready=false,tags_ready=false;
uint32_t current_player=0;uint64_t next_snapshot=0;
thread_local bool map_scope=false,custom_draw=false,drawn=false,map_input_scope=false;
thread_local tracker::Kind marker_kind=tracker::Kind::CD;
constexpr uint32_t kHead=0x83027018,kFake=0x7EDA0000;
std::vector<uint8_t> atlas_data;
bool assets_attempted=false,assets_ready=false;
std::array<uint32_t,2> asset_bitmaps{};
struct SelectorState{
    bool ready=false,focused=false,pointer=false,mouse_down=false,capture_click=false;
    float left=0,top=0,right=0,bottom=0,cursor_x=.5f,cursor_y=.5f;
    uint16_t previous=0,claimed=0;
};
SelectorState selector;std::mutex selector_mutex;
constexpr uint16_t kUp=1,kDown=2,kLeft=4,kRight=8,kAccept=0x1000;
void Log(const std::string& s){api->log(self,s.c_str());}
void ChangeMode(int direction){
    constexpr unsigned order[]={3,1,2,0};unsigned index=0;while(index<3&&order[index]!=mode.load())++index;
    mode=order[(index+(direction>0?1:3))%4];Log("Marker mode "+std::to_string(mode.load()));
}
uint16_t SelectInput(uint16_t buttons,bool mouse_down,float dx,float dy){
    // Closing, changing tabs and backing out always belong to the game.
    if(buttons&0xA330){
        selector.previous=buttons;selector.claimed=0;selector.focused=selector.pointer=selector.capture_click=false;selector.mouse_down=mouse_down;return buttons;
    }
    const auto pressed=uint16_t(buttons&~selector.previous);selector.previous=buttons;
    selector.claimed&=buttons;
    if(dx!=0||dy!=0){
        selector.pointer=true;selector.cursor_x=std::clamp(selector.cursor_x+dx,0.f,1.f);selector.cursor_y=std::clamp(selector.cursor_y+dy,0.f,1.f);
        selector.focused=selector.cursor_x>=selector.left&&selector.cursor_x<=selector.right&&selector.cursor_y>=selector.top&&selector.cursor_y<=selector.bottom;
    }
    const bool hover=selector.pointer&&selector.cursor_x>=selector.left&&selector.cursor_x<=selector.right&&selector.cursor_y>=selector.top&&selector.cursor_y<=selector.bottom;
    if(pressed&(kUp|kDown)){selector.focused=(pressed&kUp)!=0;selector.pointer=false;}
    if(selector.focused){
        const auto horizontal=pressed&(kLeft|kRight);
        if(horizontal==kLeft||horizontal==kRight)ChangeMode(horizontal==kRight?1:-1);
        selector.claimed|=buttons&(kLeft|kRight);
    }
    if(mouse_down&&!selector.mouse_down&&hover){
        selector.focused=true;selector.capture_click=true;
        ChangeMode(selector.cursor_x<(selector.left+selector.right)*.5f?-1:1);
    }
    selector.mouse_down=mouse_down;
    if(!mouse_down)selector.capture_click=false;
    return uint16_t(buttons&~(selector.claimed|kUp|kDown|(selector.capture_click?kAccept:0)));
}
bool Readable(uint32_t p,size_t n){return p>=0x10000 && uint64_t(p)+n<=UINT32_MAX && WmlHostRangeReadable(reinterpret_cast<uintptr_t>(api->guest_pointer(p)),n);}
bool SinglePlayer(){return api->read_u8(0x8370E9F6)==0 && api->read_u32(0x8309ABEC)!=0;}
bool ActiveMap(){return api->read_u32(0x8370DDAC)==26;}
void ObserveInput(WmlContext* ctx,uint8_t* base){
    const auto user=uint32_t(api->get_r(ctx,3))&255,state=uint32_t(api->get_r(ctx,5));
    float dx=0,dy=0;bool focused=false;
    HWND window=GetForegroundWindow();DWORD process=0;if(window)GetWindowThreadProcessId(window,&process);
    focused=process==GetCurrentProcessId();
    if(focused){
        RECT client{};POINT point{};
        if(GetClientRect(window,&client)&&GetCursorPos(&point)&&ScreenToClient(window,&point)&&client.right>0&&client.bottom>0){
            dx=float(point.x-client.right/2)/client.right;dy=float(point.y-client.bottom/2)/client.bottom;
        }
    }
    // The port combines keyboard/mouse and controller input here. Read mouse
    // motion before it recenters the OS cursor, then handle only this widget.
    original_input(ctx,base);
    if((user!=0&&user!=255)||api->get_r(ctx,3)!=0||!Readable(state,16))return;
    std::lock_guard lock(selector_mutex);const auto buttons=api->read_u16(state+4);
    if(!focused||!enabled||!SinglePlayer()||!ActiveMap()||!selector.ready||GetTickCount64()-map_seen.load()>100){
        selector.previous=buttons;selector.claimed=0;selector.focused=selector.pointer=selector.capture_click=false;return;
    }
    if(api->read_u8(state+6)||api->read_u8(state+7)){
        selector.previous=buttons;selector.claimed=0;selector.focused=selector.pointer=selector.capture_click=false;return;
    }
    const bool mouse_down=api->key_down(VK_LBUTTON)!=0;
    SelectInput(buttons,mouse_down,dx,dy);
    // This hook observes input only. The controller packet, button bits,
    // triggers and sticks returned by the port are never modified.
}
void MapAction(WmlContext* ctx,uint8_t* base){
    if(map_input_scope){
        const auto action=uint32_t(api->get_r(ctx,3));bool block=false;
        {std::lock_guard lock(selector_mutex);block=(selector.focused&&(action==43||action==44))||(selector.capture_click&&action==55);}
        if(block){api->set_r(ctx,3,0);return;}
    }
    original_action(ctx,base);
}
void MapPan(WmlContext* ctx,uint8_t* base){
    bool hover=false;{std::lock_guard lock(selector_mutex);hover=selector.pointer&&selector.focused;}
    if(map_input_scope&&hover&&!api->key_down('W')&&!api->key_down('A')&&!api->key_down('S')&&!api->key_down('D'))return;
    original_pan(ctx,base);
}
void MapUpdate(WmlContext* ctx,uint8_t* base){
    const bool previous=map_input_scope;map_input_scope=enabled&&SinglePlayer()&&ActiveMap();
    try{original_update(ctx,base);}catch(...){map_input_scope=previous;throw;}
    map_input_scope=previous;
}
struct Scratch{
    PPCContext ctx;uint32_t start;std::vector<uint8_t> saved;
    float opacity=1,scale_x=1,scale_y=1,anchor_x=0,anchor_y=0;
    float X(float x)const{return anchor_x+(x-anchor_x)*scale_x;}
    float Y(float y)const{return anchor_y+(y-anchor_y)*scale_y;}
    explicit Scratch(WmlContext* raw){std::memcpy(&ctx,raw,sizeof(ctx));start=ctx.r1.u32-0xC000;if(!Readable(start,0xC000))throw std::runtime_error("Map scratch stack unavailable");saved.resize(0xC000);std::memcpy(saved.data(),api->guest_pointer(start),saved.size());ctx.r1.u32-=0x8000;}
    ~Scratch(){std::memcpy(api->guest_pointer(start),saved.data(),saved.size());}
    uint32_t Data()const{return start+0x5000;}
};
void EnsureAssets(Scratch& s){
    if(assets_attempted||atlas_data.empty())return;assets_attempted=true;
    try{
        const auto bitmap_count=api->read_u32(0x8370DD60),group_count=api->read_u32(0x8370DD5C);
        if(bitmap_count>1022||group_count>=256)throw std::runtime_error("UI bitmap table full");
        // One physical allocation owns both the texture definition and its
        // aligned pixel payload for the lifetime of the game process.
        s.ctx.r3.u64=0;s.ctx.r4.u64=0x9000;s.ctx.r5.u64=4;s.ctx.r6.u64=0;s.ctx.r7.u64=uint64_t(-1);s.ctx.r8.u64=4096;
        if(api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x827889C4)!=0)throw std::runtime_error("UI texture allocation failed");
        const auto allocation=s.ctx.r3.u32;
        if(!Readable(allocation,0x9000))throw std::runtime_error("UI texture allocation unavailable");
        std::memset(api->guest_pointer(allocation),0,0x9000);
        const auto definition=allocation+0x100,pixels=allocation+0x1000;
        std::memcpy(api->guest_pointer(pixels),atlas_data.data(),atlas_data.size());
        api->write_u32(definition,pixels);api->write_u16(definition+4,128);api->write_u16(definition+6,64);
        // Uncompressed ARGB avoids BC3 block artifacts on these small badges.
        api->write_u16(definition+8,0x197);api->write_u16(definition+12,1);api->write_u8(definition+22,1);
        constexpr char name[]="collectibles_ui.tga";
        std::memcpy(api->guest_pointer(definition+23),name,sizeof(name));
        s.ctx.r3.u64=definition;
        if(api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x82644988)!=0)throw std::runtime_error("UI texture registration failed");
        s.ctx.r3.u64=definition+23;
        if(api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x82644C60)!=0||s.ctx.r3.s32<0)throw std::runtime_error("UI texture lookup failed");
        const auto texture=s.ctx.r3.u32;
        const auto group=0x82FFAEC8+group_count*8;
        api->write_u32(group,definition+23);api->write_u32(group+4,texture);
        for(unsigned i=0;i<2;++i){
            asset_bitmaps[i]=bitmap_count+i;const auto entry=0x82FF5EC8+asset_bitmaps[i]*20;
            api->write_u32(entry,group_count);api->write_u32(entry+4,i==0?1:41);api->write_u32(entry+8,1);
            api->write_u32(entry+12,38);api->write_u32(entry+16,38);
        }
        api->write_u32(0x8370DD5C,group_count+1);api->write_u32(0x8370DD60,bitmap_count+2);
        assets_ready=true;Log("Loaded native-resolution CD and graffiti textures");
    }catch(const std::exception& e){Log(std::string("Custom UI textures unavailable; using native marker fallback: ")+e.what());}
}
void Snapshot(){
    const uint64_t now=GetTickCount64();const auto p=api->read_u32(0x8309ABEC);
    if(p==current_player && now<next_snapshot)return;current_player=p;next_snapshot=now+250;
    cds_ready=tags_ready=false;missing.clear();
    if(!SinglePlayer()||!Readable(p,21232))return;
    std::unordered_set<uint32_t> cd_ids,tag_ids,tag_seen;
    std::unordered_map<uint32_t,tracker::Kind> known;for(const auto& l:locations)known.emplace(l.id,l.kind);
    cds=api->read_u32(p+21228);cds_ready=cds<=60;
    if(cds_ready)for(unsigned i=0;i<cds;++i){auto id=api->read_u32(p+20988+4*i);auto it=known.find(id);if(it==known.end()||it->second!=tracker::Kind::CD){cds_ready=false;break;}cd_ids.insert(id);}
    if(cds_ready && cd_ids.size()!=cds)cds_ready=false;
    const auto head=api->read_u32(0x827E2644);auto node=head;std::unordered_set<uint32_t> visited;
    while(node && visited.size()<256 && visited.insert(node).second){
        if(!Readable(node,28))break;auto def=api->read_u32(node+4);if(!Readable(def,64))break;
        auto id=api->read_u32(def);auto it=known.find(id);
        if(it!=known.end()&&it->second==tracker::Kind::Graffiti){tag_seen.insert(id);if(api->read_u8(node)&0x40)tag_ids.insert(id);}
        node=api->read_u32(node+20);
    }
    tags=unsigned(tag_ids.size());tags_ready=tag_seen.size()==75;
    for(const auto& l:locations){if(l.kind==tracker::Kind::CD){if(cds_ready&&!cd_ids.contains(l.id))missing.push_back(l);}else if(tags_ready&&!tag_ids.contains(l.id))missing.push_back(l);}
    static int last_cd=-1,last_tag=-1;
    if(int(cds)!=last_cd||int(tags)!=last_tag){Log("Progress: CDs "+std::to_string(cds)+"/60, graffiti "+std::to_string(tags)+"/75; missing locations "+std::to_string(missing.size()));last_cd=int(cds);last_tag=int(tags);}
}
void Position(WmlContext* ctx,uint8_t* base){
    if(custom_draw){const auto node=uint32_t(api->get_r(ctx,3)),out=uint32_t(api->get_r(ctx,4));
        if(Readable(node,48)){auto handle=api->read_u32(node+16);auto index=handle&255;
            if((handle&0xFFFFFF00u)==kFake && index<missing.size() && api->read_u32(node+12)==6){
                marker_kind=missing[index].kind;if(out)for(unsigned i=0;i<3;++i)api->write_f32(out+4*i,missing[index].pos[i]);api->set_r(ctx,3,1);return;
            }
        }
    }
    original_pos(ctx,base);
}
void Color(WmlContext* ctx,uint8_t* base){
    if(custom_draw){const bool cd=marker_kind==tracker::Kind::CD;api->set_r(ctx,3,assets_ready?255:cd?255:190);api->set_r(ctx,4,assets_ready?255:cd?220:105);api->set_r(ctx,5,assets_ready?255:cd?64:255);}
    original_color(ctx,base);
}
void Bitmap(WmlContext* ctx,uint8_t* base){
    if(custom_draw&&assets_ready)api->set_r(ctx,3,asset_bitmaps[marker_kind==tracker::Kind::CD?0:1]);
    original_bitmap(ctx,base);
}
bool InsideMinimap(const tracker::Location& location){
    const float x=api->read_f32(0x83A08418),z=api->read_f32(0x83A08420),zoom=api->read_f32(0x827AF11C);
    const float hud=std::max(api->read_f32(0x82FFB4E8),api->read_f32(0x82FFB4EC));
    const float radius=api->read_f32(0x82089770)*hud-(assets_ready?21.f:14.f)*hud;
    if(!std::isfinite(x)||!std::isfinite(z)||!std::isfinite(zoom)||zoom<=0||!std::isfinite(radius)||radius<=0)return false;
    const float dx=(location.pos[0]-x)*zoom,dz=(location.pos[2]-z)*zoom;
    return dx*dx+dz*dz<=radius*radius;
}
void DrawMarkers(WmlContext* raw,uint8_t* base,const PPCContext* mini_args=nullptr){
    try{
        Scratch scratch(raw);EnsureAssets(scratch);auto nodes=scratch.Data();unsigned count=0;
        for(unsigned i=0;i<missing.size();++i){if(!tracker::Visible(missing[i].kind,mode)||(mini_args&&!InsideMinimap(missing[i])))continue;
            auto node=nodes+count*48;std::memset(api->guest_pointer(node),0,48);
            api->write_u32(node+8,0x7000+i);api->write_u32(node+12,6);api->write_u32(node+16,kFake|i);
            api->write_u32(node+32,59);api->write_u32(node+36,0xFFFFFFFFu);
            // Minimap collectibles use native distance culling, rather than
            // pinning every distant location to the edge of the radar.
            api->write_u8(node+44,mini_args?0:0x80);++count;
        }
        if(!count)return;
        for(unsigned i=0;i<count;++i){api->write_u32(nodes+i*48,nodes+((i+count-1)%count)*48);api->write_u32(nodes+i*48+4,nodes+((i+1)%count)*48);}
        auto color=scratch.Data()+0x2000;scratch.ctx.r3.u64=color;
        if(api->call(reinterpret_cast<WmlContext*>(&scratch.ctx),0x8251E4E8)!=0)throw std::runtime_error("Map color state unavailable");
        std::array<uint8_t,4> saved_color;for(unsigned i=0;i<4;++i)saved_color[i]=api->read_u8(color+i);
        const uint32_t previous=api->read_u32(kHead);
        auto restore_color=[&]{for(unsigned i=0;i<4;++i)api->set_r(reinterpret_cast<WmlContext*>(&scratch.ctx),3+i,saved_color[i]);original_color(reinterpret_cast<WmlContext*>(&scratch.ctx),base);};
        // The extra draw uses a temporary list. Restore the native mission
        // list before returning; no icons enter its pool or save data.
        custom_draw=true;api->write_u32(kHead,nodes);
        scratch.ctx.r3.u64=4;
        if(mini_args){scratch.ctx.r6=mini_args->r6;scratch.ctx.r7=mini_args->r7;scratch.ctx.r8=mini_args->r8;scratch.ctx.f1=mini_args->f1;}
        else{scratch.ctx.r4.u64=0;scratch.ctx.r5.u64=1;}
        try{(mini_args?original_minibatch:original_batch)(reinterpret_cast<WmlContext*>(&scratch.ctx),base);}
        catch(...){api->write_u32(kHead,previous);custom_draw=false;restore_color();throw;}
        api->write_u32(kHead,previous);custom_draw=false;
        restore_color();
    }catch(const std::exception& e){custom_draw=false;enabled=false;Log(std::string("Tracker disabled: ")+e.what());}
}
void Batch(WmlContext* raw,uint8_t* base){
    original_batch(raw,base);
    if(!enabled||!map_scope||custom_draw||drawn||!mode.load()||!SinglePlayer())return;
    drawn=true;DrawMarkers(raw,base);
}
void MiniBatch(WmlContext* raw,uint8_t* base){
    PPCContext args;std::memcpy(&args,raw,sizeof(args));
    original_minibatch(raw,base);
    if(!enabled||custom_draw||map_scope||args.r3.u32!=4||!mode.load()||!SinglePlayer())return;
    try{Snapshot();DrawMarkers(raw,base,&args);}
    catch(const std::exception& e){enabled=false;Log(e.what());}
}
void PanelColor(Scratch& s,unsigned r,unsigned g,unsigned b,unsigned a=255){
    s.ctx.r3.u64=r;s.ctx.r4.u64=g;s.ctx.r5.u64=b;s.ctx.r6.u64=unsigned(std::round(a*s.opacity));
    api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x8263E7D8);
}
void PanelRect(Scratch& s,float x,float y,float w,float h,unsigned r,unsigned g,unsigned b,unsigned a){
    PanelColor(s,r,g,b,a);s.ctx.r3.u64=int32_t(s.X(x));s.ctx.r4.u64=int32_t(s.Y(y));s.ctx.r5.u64=int32_t(w*s.scale_x);s.ctx.r6.u64=int32_t(h*s.scale_y);s.ctx.r7.u64=0x82839AA0;
    api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x8263F4D0);
}
void PanelRoundedRect(Scratch& s,float x,float y,float w,float h,float radius,unsigned r,unsigned g,unsigned b,unsigned a){
    const int rows=std::max(1,int(std::round(h)));radius=std::min(radius,std::min(w,h)*.5f);
    int begin=0,last=-1;
    for(int row=0;row<=rows;++row){
        int inset=-1;
        if(row<rows){const float edge=std::min(row+.5f,h-row-.5f);inset=edge<radius?int(std::ceil(radius-std::sqrt(std::max(0.f,radius*radius-(radius-edge)*(radius-edge))))):0;}
        if(row && inset!=last){PanelRect(s,x+last,y+begin,w-2*last,float(row-begin),r,g,b,a);begin=row;}
        last=inset;
    }
}
bool PanelBitmap(Scratch& s,uint32_t bitmap,float x,float y,float w,float h){
    if(bitmap>=4096)return false;auto info=0x82FF5EC8u+bitmap*20;
    if(!Readable(info,20))return false;auto width=api->read_u32(info+12),height=api->read_u32(info+16);
    if(!width||!height||width>8192||height>8192)return false;
    s.ctx.r3.u64=bitmap;s.ctx.r8.u64=0;s.ctx.r9.u64=0;s.ctx.r10.u64=width;
    s.ctx.f1.f64=s.X(x);s.ctx.f2.f64=s.Y(y);s.ctx.f3.f64=w*s.scale_x;s.ctx.f4.f64=h*s.scale_y;
    api->write_u32(s.ctx.r1.u32+84,height);api->write_u8(s.ctx.r1.u32+95,0);api->write_u8(s.ctx.r1.u32+103,0);api->write_u32(s.ctx.r1.u32+108,0x82839AA0);
    return api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x8227E450)==0;
}
void PanelText(Scratch& s,const std::string& text,float x,float y,float scale,std::array<unsigned,3> color,bool title=false,bool right=false,bool center=false){
    x=s.X(x);y=s.Y(y);scale*=std::min(s.scale_x,s.scale_y);
    auto data=s.Data();auto buffer=data+64;
    for(unsigned i=0;i<128;++i)api->write_u16(buffer+i*2,i<text.size()?uint8_t(text[i]):0);
    const auto fill=api->read_u32(title?0x827D6D14:0x827D6D1C),outline=api->read_u32(title?0x827D6D10:0x827D6D20);
    if(!fill||!outline)return;
    s.ctx.r3.u64=data;s.ctx.r4.u64=data+4;s.ctx.r5.u64=buffer;s.ctx.r6.u64=uint64_t(-1);s.ctx.r7.u64=outline;
    if(api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x8264EF48)!=0)return;
    const float width=float(api->read_u32(data))*scale,height=float(api->read_u32(data+4))*scale;
    PanelColor(s,color[0],color[1],color[2]);
    for(auto font:{outline,fill}){
        s.ctx.r3.u64=int32_t(right?x-width:center?x-width*.5f:x);s.ctx.r4.u64=int32_t(y-height*.5f);s.ctx.r5.u64=buffer;s.ctx.r6.u64=0;s.ctx.r7.u64=font;s.ctx.r8.u64=0x82839AA0;s.ctx.f1.f64=scale;
        api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x8264E988);
    }
}
void DrawPanel(WmlContext* raw,float map_left,float map_top,float map_right,float map_bottom,float opacity){
    if(!std::isfinite(opacity)||opacity<=.005f)return;
    Scratch s(raw);EnsureAssets(s);s.ctx.r3.u64=s.Data()+512;
    if(api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x8251E4E8)!=0)return;
    std::array<unsigned,4> saved;for(unsigned i=0;i<4;++i)saved[i]=api->read_u8(s.Data()+512+i);
    if(api->call(reinterpret_cast<WmlContext*>(&s.ctx),0x8263D818)!=0)return;
    auto state=s.ctx.r3.u32;if(!Readable(state,8))return;
    const float sw=float(api->read_u32(state)),sh=float(api->read_u32(state+4));
    float ui=std::max(api->read_f32(0x82FFB4E8),api->read_f32(0x82FFB4EC));if(!std::isfinite(ui)||ui<.1f||ui>8)ui=1;
    if(!std::isfinite(map_left)||map_left<0||map_left>sw)map_left=sw*.28f;
    if(!std::isfinite(map_top)||map_top<0||map_top>sh)map_top=sh*.24f;
    if(std::isfinite(map_right)&&std::isfinite(map_bottom)&&map_right>map_left&&map_bottom>map_top){
        s.anchor_x=(map_left+map_right)*.5f;s.anchor_y=(map_top+map_bottom)*.5f;
        s.scale_x=std::clamp((map_right-map_left)/(620.f*ui),.001f,2.f);
        s.scale_y=std::clamp((map_bottom-map_top)/(340.f*ui),.001f,2.f);
        map_left=s.anchor_x-310.f*ui;map_top=s.anchor_y-170.f*ui;
    }
    s.opacity=std::clamp(opacity,0.f,1.f);
    const float w=std::min(244.f*ui,map_left-40.f*ui),h=224.f*ui;
    if(w<180.f*ui)return;
    const float x=std::max(16.f*ui,(map_left-w)*.5f),y=std::min(map_top+16.f*ui,sh-h-70.f*ui),pad=16.f*ui;
    PanelColor(s,255,255,255,230);
    if(!PanelBitmap(s,api->read_u32(0x827B05E8),x,y,w,h))PanelRect(s,x,y,w,h,0,0,0,205);
    PanelRect(s,x,y,w,2*ui,130,130,130,180);PanelRect(s,x,y+h-2*ui,w,2*ui,130,130,130,180);
    const std::array<unsigned,3> white{240,240,240},gold{255,216,74},grey{175,175,175},yellow{255,220,64},purple{190,105,255};
    PanelText(s,"COLLECTIBLES",x+pad,y+28*ui,.56f*ui,gold,true);
    PanelRect(s,x+pad,y+51*ui,w-2*pad,ui,150,150,150,120);
    PanelText(s,"CDs",x+pad,y+78*ui,.82f*ui,yellow);
    PanelText(s,cds_ready?std::to_string(cds)+" / 60":"-- / 60",x+w-pad,y+78*ui,.82f*ui,white,false,true);
    PanelText(s,"Graffiti",x+pad,y+111*ui,.82f*ui,purple);
    PanelText(s,tags_ready?std::to_string(tags)+" / 75":"-- / 75",x+w-pad,y+111*ui,.82f*ui,white,false,true);
    PanelRect(s,x+pad,y+139*ui,w-2*pad,ui,150,150,150,120);
    SelectorState view;
    {std::lock_guard lock(selector_mutex);
        selector.left=s.X(x+pad)/sw;selector.right=s.X(x+w-pad)/sw;
        selector.top=s.Y(y+171*ui)/sh;selector.bottom=s.Y(y+201*ui)/sh;
        selector.ready=opacity>.99f&&std::abs(s.scale_x-1)<.02f&&std::abs(s.scale_y-1)<.02f;
        view=selector;
    }
    PanelText(s,"MARKERS",x+pad,y+157*ui,.62f*ui,white);
    PanelRoundedRect(s,x+pad,y+171*ui,w-2*pad,30*ui,3*ui,15,15,18,155);
    if(view.focused){
        PanelRect(s,x+pad+3*ui,y+171*ui,w-2*pad-6*ui,ui,220,190,96,180);
        PanelRect(s,x+pad+3*ui,y+200*ui,w-2*pad-6*ui,ui,220,190,96,180);
    }
    const char* names[]={"OFF","CDs","GRAFFITI","ALL"};
    const std::array<unsigned,3> ink=view.focused?white:gold;
    PanelText(s,"<",x+pad+9*ui,y+186*ui,.72f*ui,ink);
    PanelText(s,names[mode.load()],x+w*.5f,y+186*ui,.72f*ui,ink,false,false,true);
    PanelText(s,">",x+w-pad-9*ui,y+186*ui,.72f*ui,ink,false,true);
    PanelText(s,view.focused?"DOWN: MAP FILTERS":"UP: SELECT MARKERS",x+w*.5f,y+214*ui,.42f*ui,grey,false,false,true);
    if(view.pointer&&view.ready){
        // A small native-rendered pointer follows the captured mouse. It is
        // drawn after the panel transform, in final viewport coordinates.
        const float old_x=s.scale_x,old_y=s.scale_y;s.scale_x=s.scale_y=1;
        const float px=view.cursor_x*sw,py=view.cursor_y*sh;
        for(int row=0;row<12;++row){const float width=std::min(7,row/2+1)*ui;PanelRect(s,px,py+row*ui,width,ui,0,0,0,255);if(row>1&&width>2*ui)PanelRect(s,px+ui,py+row*ui,width-2*ui,ui,245,245,245,255);}
        s.scale_x=old_x;s.scale_y=old_y;
    }
    s.opacity=1;PanelColor(s,saved[0],saved[1],saved[2],saved[3]);
}
void Map(WmlContext* ctx,uint8_t* base){
    if(!enabled||!SinglePlayer()||!ActiveMap()){original_map(ctx,base);return;}
    try{Snapshot();}catch(const std::exception& e){enabled=false;Log(e.what());original_map(ctx,base);return;}
    float left=-1,top=-1,right=-1,bottom=-1;
    const auto tl=uint32_t(api->get_r(ctx,5)),br=uint32_t(api->get_r(ctx,6));
    if(Readable(tl,8)){left=api->read_f32(tl);top=api->read_f32(tl+4);}
    if(Readable(br,8)){right=api->read_f32(br);bottom=api->read_f32(br+4);}
    const float opacity=float(reinterpret_cast<PPCContext*>(ctx)->f1.f64);
    map_seen=GetTickCount64();const bool previous=map_scope;map_scope=true;drawn=false;
    original_map(ctx,base);map_scope=previous;
    if(enabled)try{DrawPanel(ctx,left,top,right,bottom,opacity);}catch(const std::exception& e){enabled=false;Log(e.what());}
}
void Frame(void*){
    const bool visible=enabled&&SinglePlayer()&&GetTickCount64()-map_seen.load()<250;
    if(!visible){std::lock_guard lock(selector_mutex);selector.ready=selector.focused=selector.pointer=selector.capture_click=false;}
}
}
extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader,const WmlMod* mod){
    api=loader;self=mod;if(!api||api->version!=1||api->size<offsetof(WmlApi,overlay_text)+sizeof(api->overlay_text)||!api->overlay_text)return 1;
    try{
        const auto folder=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(mod->folder)));
        locations=tracker::Pack(folder.parent_path().parent_path()/"game"/"packfiles"/"misc.vpp_xbox2").Locations();
        auto ini=folder/"mod.ini";mode=GetPrivateProfileIntW(L"settings",L"markers",3,ini.c_str())&3;
        std::ifstream atlas(folder/"assets"/"ui-atlas.bin",std::ios::binary);
        atlas_data.assign(std::istreambuf_iterator<char>(atlas),{});
        if(atlas_data.size()!=32768){atlas_data.clear();Log("UI atlas missing or invalid; using native marker fallback");}
        Log("Read 60 CD and 75 graffiti locations from the installed game");
    }catch(const std::exception& e){Log(e.what());return 1;}
    struct Hook{uint32_t address;WmlGuestFunction replacement;WmlGuestFunction* previous;};
    Hook hooks[]={{0x822B3F70,Map,&original_map},{0x822EDC50,Batch,&original_batch},{0x822E98B8,MiniBatch,&original_minibatch},{0x822EE5E8,Position,&original_pos},{0x8263E7D8,Color,&original_color},{0x8227E720,Bitmap,&original_bitmap},{0x82788EC4,ObserveInput,&original_input},{0x822B6548,MapUpdate,&original_update},{0x8216EE80,MapAction,&original_action},{0x822B6160,MapPan,&original_pan}};
    bool good=true;for(const auto& h:hooks)if(api->hook(h.address,h.replacement,h.previous)){good=false;Log("Required map hook unavailable; tracker disabled");break;}
    enabled=good;if(good){api->on_frame(Frame,nullptr);Log("Ready: map-side marker selector; mouse arrows or D-pad Up then Left/Right, Down returns to map filters");}
    return 0;
}
