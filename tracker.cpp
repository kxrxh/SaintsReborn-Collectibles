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
WmlGuestFunction original_map,original_batch,original_minibatch,original_pos,original_color,original_bitmap;
std::vector<tracker::Location> locations,missing;
std::atomic<bool> enabled{false};std::atomic<unsigned> mode{3};
std::atomic<uint64_t> map_seen{0};
unsigned cds=0,tags=0;bool cds_ready=false,tags_ready=false;
uint32_t current_player=0;uint64_t next_snapshot=0;
thread_local bool map_scope=false,custom_draw=false,drawn=false;
thread_local tracker::Kind marker_kind=tracker::Kind::CD;
constexpr uint32_t kHead=0x83027018,kFake=0x7EDA0000;
std::vector<uint8_t> atlas_data;
bool assets_attempted=false,assets_ready=false;
std::array<uint32_t,3> asset_bitmaps{};
void Log(const std::string& s){api->log(self,s.c_str());}
bool Readable(uint32_t p,size_t n){return p>=0x10000 && uint64_t(p)+n<=UINT32_MAX && WmlHostRangeReadable(reinterpret_cast<uintptr_t>(api->guest_pointer(p)),n);}
bool SinglePlayer(){return api->read_u8(0x8370E9F6)==0 && api->read_u32(0x8309ABEC)!=0;}
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
        if(bitmap_count>1021||group_count>=256)throw std::runtime_error("UI bitmap table full");
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
        for(unsigned i=0;i<3;++i){
            asset_bitmaps[i]=bitmap_count+i;const auto entry=0x82FF5EC8+asset_bitmaps[i]*20;
            api->write_u32(entry,group_count);api->write_u32(entry+4,i==0?1:i==1?41:81);api->write_u32(entry+8,1);
            api->write_u32(entry+12,i==2?32:38);api->write_u32(entry+16,i==2?32:38);
        }
        api->write_u32(0x8370DD5C,group_count+1);api->write_u32(0x8370DD60,bitmap_count+3);
        assets_ready=true;Log("Loaded native-resolution CD, graffiti and F7 textures");
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
void PanelText(Scratch& s,const std::string& text,float x,float y,float scale,std::array<unsigned,3> color,bool title=false,bool right=false){
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
        s.ctx.r3.u64=int32_t(right?x-width:x);s.ctx.r4.u64=int32_t(y-height*.5f);s.ctx.r5.u64=buffer;s.ctx.r6.u64=0;s.ctx.r7.u64=font;s.ctx.r8.u64=0x82839AA0;s.ctx.f1.f64=scale;
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
    // Match the port's 32-pixel keyboard prompts: rounded pale rim,
    // dark inset face and a small drop shadow.
    PanelColor(s,255,255,255);
    if(!assets_ready||!PanelBitmap(s,asset_bitmaps[2],x+pad,y+151*ui,32*ui,32*ui)){
        PanelRoundedRect(s,x+pad+ui,y+154*ui,30*ui,30*ui,6*ui,0,0,0,170);
        PanelRoundedRect(s,x+pad+ui,y+152*ui,30*ui,30*ui,6*ui,236,236,236,255);
        PanelRoundedRect(s,x+pad+3*ui,y+154*ui,26*ui,25*ui,4*ui,52,52,56,255);
        PanelText(s,"F7",x+pad+6*ui,y+166*ui,.59f*ui,white);
    }
    PanelText(s,"MARKERS",x+pad+45*ui,y+167*ui,.70f*ui,white);
    const char* names[]={"OFF","CDs","GRAFFITI","ALL"};
    PanelText(s,names[mode.load()],x+pad,y+200*ui,.72f*ui,gold);
    s.opacity=1;PanelColor(s,saved[0],saved[1],saved[2],saved[3]);
}
void Map(WmlContext* ctx,uint8_t* base){
    if(!enabled||!SinglePlayer()){original_map(ctx,base);return;}
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
    if(visible&&api->key_pressed(VK_F7)){static const unsigned next[]={3,2,0,1};mode=next[mode.load()];Log("Marker mode "+std::to_string(mode.load()));}
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
    Hook hooks[]={{0x822B3F70,Map,&original_map},{0x822EDC50,Batch,&original_batch},{0x822E98B8,MiniBatch,&original_minibatch},{0x822EE5E8,Position,&original_pos},{0x8263E7D8,Color,&original_color},{0x8227E720,Bitmap,&original_bitmap}};
    bool good=true;for(const auto& h:hooks)if(api->hook(h.address,h.replacement,h.previous)){good=false;Log("Required map hook unavailable; tracker disabled");break;}
    enabled=good;if(good){api->on_frame(Frame,nullptr);Log("Ready: open the map; F7 cycles Both / CDs / Graffiti / Off");}
    return 0;
}
