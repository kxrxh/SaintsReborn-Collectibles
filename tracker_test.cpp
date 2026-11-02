#include "tracker.cpp"
#include <cassert>
#include <iostream>
#include <map>
#include <limits>
namespace {
std::map<uint32_t,std::vector<uint8_t>> memory;
WmlApi mock{};int native_batches=0,extra_batches=0,marker_draws=0,pos_fallbacks=0,native_minis=0,extra_minis=0;bool fail_draw=false,pressed=false,mini_pass=false;
std::array<unsigned,4> color_state{11,22,33,144};std::string overlay;
std::vector<std::string> labels;
uint32_t drawn_bitmap=0;
std::vector<std::array<int,5>> rectangles;
void* Pointer(uint32_t a){auto it=memory.upper_bound(a);if(it==memory.begin())return nullptr;--it;auto n=uint64_t(a)-it->first;return n<it->second.size()?it->second.data()+n:nullptr;}
uint8_t U8(uint32_t p){auto a=Pointer(p);assert(a);return *static_cast<uint8_t*>(a);}
uint16_t U16(uint32_t p){return uint16_t(U8(p))<<8|U8(p+1);}
uint32_t U32(uint32_t p){return uint32_t(U16(p))<<16|U16(p+2);}
float Float(uint32_t p){return std::bit_cast<float>(U32(p));}
void W8(uint32_t p,uint8_t v){auto a=Pointer(p);assert(a);*static_cast<uint8_t*>(a)=v;}
void W16(uint32_t p,uint16_t v){W8(p,uint8_t(v>>8));W8(p+1,uint8_t(v));}
void W32(uint32_t p,uint32_t v){W16(p,uint16_t(v>>16));W16(p+2,uint16_t(v));}
void WF(uint32_t p,float v){W32(p,std::bit_cast<uint32_t>(v));}
int Index(int i){return i==3?0:(i<3?i+1:i);}
uint64_t R(WmlContext* c,int i){return reinterpret_cast<PPCRegister*>(c)[Index(i)].u64;}
void RS(WmlContext* c,int i,uint64_t v){reinterpret_cast<PPCRegister*>(c)[Index(i)].u64=v;}
void LogFake(const WmlMod*,const char*){}
void Overlay(const char* s){overlay=s;}
int Key(int key){assert(key==VK_F7);bool result=pressed;pressed=false;return result;}
void NativeColor(WmlContext* c,uint8_t*){for(int i=0;i<4;++i)color_state[i]=unsigned(R(c,i+3));}
int Call(WmlContext* c,uint32_t fn){
    if(fn==0x827889C4){assert(R(c,3)==0&&R(c,4)==0x9000&&R(c,5)==4&&R(c,8)==4096);RS(c,3,0xA0000000);}
    else if(fn==0x82644988){auto d=uint32_t(R(c,3));assert(U32(d)==0xA0001000&&U16(d+4)==128&&U16(d+6)==64&&U16(d+8)==0x197&&U16(d+12)==1&&U8(d+22)==1);}
    else if(fn==0x82644C60)RS(c,3,1737);
    else if(fn==0x8251E4E8){auto p=uint32_t(R(c,3));for(unsigned i=0;i<4;++i)W8(p+i,uint8_t(color_state[i]));}
    else if(fn==0x8263D818)RS(c,3,0x90012000);
    else if(fn==0x8263E7D8)Color(c,nullptr);
    else if(fn==0x8264EF48){auto p=uint32_t(R(c,5));unsigned n=0;while(U16(p+n*2))++n;W32(uint32_t(R(c,3)),n*12);W32(uint32_t(R(c,4)),24);}
    else if(fn==0x8264E988){std::string text;auto p=uint32_t(R(c,5));for(unsigned i=0;i<128 && U16(p+i*2);++i)text+=char(U16(p+i*2));labels.push_back(text);}
    else if(fn==0x8263F4D0)rectangles.push_back({int(R(c,3)),int(R(c,4)),int(R(c,5)),int(R(c,6)),int(color_state[3])});
    else assert(false && "Unexpected native UI function");
    return 0;
}
void NativePos(WmlContext*,uint8_t*){++pos_fallbacks;}
void NativeBitmap(WmlContext* c,uint8_t*){drawn_bitmap=uint32_t(R(c,3));}
void NativeAction(WmlContext* c,uint8_t*){RS(c,3,1);}
void NativeBatch(WmlContext* raw,uint8_t*){
    if(!custom_draw){++native_batches;return;}
    ++extra_batches;
    assert(R(raw,3)==4 && R(raw,4)==0);auto head=U32(kHead),node=head;unsigned count=0;
    do{
        assert(node && ++count<=135);auto handle=U32(node+16),i=handle&255;assert(i<missing.size());
        assert(U8(node+44)==(mini_pass?0:0x80));
        const auto out=uint32_t(R(raw,1))+0x3F00;
        RS(raw,3,node);RS(raw,4,out);Position(raw,nullptr);assert(R(raw,3)==1);
        for(int j=0;j<3;++j)assert(Float(out+4*j)==missing[i].pos[j]);
        RS(raw,3,255);RS(raw,4,255);RS(raw,5,255);RS(raw,6,200);Color(raw,nullptr);
        bool cd=missing[i].kind==tracker::Kind::CD;assert(color_state[0]==unsigned(cd?255:190) && color_state[3]==200);
        if(fail_draw)throw std::runtime_error("Simulated renderer failure");
        ++marker_draws;node=U32(node+4);
    }while(node!=head);
}
void NativeMini(WmlContext* raw,uint8_t* base){
    if(!custom_draw){++native_minis;return;}
    ++extra_minis;auto c=reinterpret_cast<PPCContext*>(raw);
    assert(R(raw,3)==4&&R(raw,6)==0x44&&R(raw,7)==0x55&&R(raw,8)==0x66&&c->f1.f64==.75);
    mini_pass=true;RS(raw,4,0);
    try{NativeBatch(raw,base);}catch(...){mini_pass=false;throw;}
    mini_pass=false;
}
void NativeMap(WmlContext* c,uint8_t* b){Batch(c,b);Batch(c,b);}
}
int main(int argc,char** argv){
    assert(argc==2);auto game=std::filesystem::path(argv[1]);
    assert(tracker::NameId("123456789")==0x2DFD2D88u);assert(tracker::NameId("A")!=tracker::NameId("a"));
    std::vector<tracker::Location> sample;
    tracker::ParseScript("test.cts","#Navpoints\n$Navpoint: \"test_cd\"\n$Pos: <1, 2, 3>\n#Items\n$Item: \"sample\"\n$Item type: \"CD_collect\"\n$Start nav: \"test_cd\"\n#End\n",sample);
    assert(sample.size()==1&&sample[0].kind==tracker::Kind::CD&&sample[0].pos[2]==3);
    tracker::ParseScript("lc_tagging.cts","#Tagging Spots\n$Tagging Spot: \"sample_tag\"\n$Pos: <4 5 6>\n+Ambient\n#End",sample);
    assert(sample.size()==2&&sample[1].kind==tracker::Kind::Graffiti);
    locations=tracker::Pack(game/"dist"/"game"/"packfiles"/"misc.vpp_xbox2").Locations();assert(locations.size()==135);
    bool invalid=false;try{tracker::Validate(sample);}catch(...){invalid=true;}assert(invalid);
    for(unsigned m=0;m<4;++m){assert(tracker::Visible(tracker::Kind::CD,m)==bool(m&1));assert(tracker::Visible(tracker::Kind::Graffiti,m)==bool(m&2));}
    memory[0x82700000].resize(0x100000);memory[0x82FF0000].resize(0x10000);memory[0x83000000].resize(0x100000);memory[0x83700000].resize(0x20000);
    memory[0x90000000].resize(0x20000);memory[0x91000000].resize(0x20000);memory[0x70000000].resize(0x20000);
    memory[0x83A00000].resize(0x10000);memory[0x839E0000].resize(0x10000);memory[0x82080000].resize(0x10000);
    mock.version=1;mock.size=sizeof(mock);mock.log=LogFake;mock.read_u8=U8;mock.read_u16=U16;mock.read_u32=U32;mock.read_f32=Float;
    mock.write_u8=W8;mock.write_u16=W16;mock.write_u32=W32;mock.write_f32=WF;mock.guest_pointer=Pointer;mock.get_r=R;mock.set_r=RS;
    mock.call=Call;mock.overlay_text=Overlay;mock.key_pressed=Key;api=&mock;static WmlMod mod{"Collectibles","test"};self=&mod;
    original_map=NativeMap;original_batch=NativeBatch;original_minibatch=NativeMini;original_pos=NativePos;original_color=NativeColor;original_bitmap=NativeBitmap;original_action=NativeAction;enabled=true;
    W32(0x90012000,1280);W32(0x90012004,720);WF(0x82FFB4E8,1);WF(0x82FFB4EC,1);
    WF(0x82089770,67);WF(0x827AF11C,.000001f);
    W32(0x827D6D1C,1);W32(0x827D6D20,2);W32(0x827D6D14,3);W32(0x827D6D10,4);
    W32(0x8309ABEC,0x90000000);W32(0x90000000+21228,1);
    W32(0x8370DDAC,26);
    for(const auto& l:locations)if(l.kind==tracker::Kind::CD){W32(0x90000000+20988,l.id);break;}
    unsigned tag=0;for(const auto& l:locations)if(l.kind==tracker::Kind::Graffiti){auto n=0x91000000+tag*28,d=0x91010000+tag*64;W32(n+4,d);W32(d,l.id);W8(n,tag<2?0x40:0);W32(n+20,0x91000000+((tag+1)%75)*28);++tag;}
    W32(0x827E2644,0x91000000);W32(kHead,0x90002000);
    Snapshot();assert(cds_ready&&tags_ready&&cds==1&&tags==2&&missing.size()==132);
    const auto saved=memory;const auto colors=color_state;PPCContext ctx{};ctx.r1.u32=0x70018000;ctx.f1.f64=1;
    const auto before=ctx;auto raw=reinterpret_cast<WmlContext*>(&ctx);mode=3;
    Map(raw,nullptr);assert(extra_batches==1&&native_batches==2&&marker_draws==132&&pos_fallbacks==0);
    assert(U32(kHead)==0x90002000&&color_state==colors&&std::memcmp(&ctx,&before,sizeof(ctx))==0);
    assert(memory==saved); // no progress, mission-list or stack changes survive the draw
    Frame(nullptr);assert(overlay.empty());assert(std::find(labels.begin(),labels.end(),"1 / 60")!=labels.end()&&std::find(labels.begin(),labels.end(),"2 / 75")!=labels.end());
    selector={};selector.ready=true;selector.left=.1f;selector.right=.3f;selector.top=.7f;selector.bottom=.9f;
    assert(SelectInput(kRight,false,0,0)==kRight&&mode==3);SelectInput(0,false,0,0);
    assert(SelectInput(kUp,false,0,0)==0&&selector.focused);SelectInput(0,false,0,0);
    assert(SelectInput(kRight,false,0,0)==0&&mode==1);SelectInput(kRight,false,0,0);assert(mode==1);
    SelectInput(0,false,0,0);SelectInput(kRight,false,0,0);assert(mode==2);
    SelectInput(0,false,0,0);SelectInput(kRight,false,0,0);assert(mode==0);
    SelectInput(0,false,0,0);SelectInput(kLeft,false,0,0);assert(mode==2);
    SelectInput(0,false,0,0);SelectInput(kDown,false,0,0);assert(!selector.focused);
    assert(SelectInput(kRight,false,0,0)==kRight&&mode==2);SelectInput(0,false,0,0);
    SelectInput(0,false,-.25f,.3f);assert(selector.focused&&selector.pointer);
    assert(SelectInput(kAccept,true,0,0)==0&&mode==0); // click right half cycles once
    assert(SelectInput(kAccept,true,.5f,0)==0&&mode==0); // dragging out cannot place a waypoint
    SelectInput(0,false,0,0);assert(SelectInput(kAccept,true,0,0)==kAccept); // outside widget: normal map click
    for(const auto navigation:{0x10,0x20,0x100,0x200,0x2000,0x8000}){
        selector.focused=selector.pointer=selector.capture_click=true;
        assert(SelectInput(uint16_t(navigation),true,0,0)==navigation&&!selector.focused&&!selector.capture_click);
    }
    W32(0x8370DDAC,46);auto previous_batches=extra_batches;Map(raw,nullptr);assert(extra_batches==previous_batches);
    W32(0x8370DDAC,26);
    selector={};mode=3;
    const auto batches=extra_batches;mode=0;Map(raw,nullptr);assert(extra_batches==batches);
    mode=1;marker_draws=0;Map(raw,nullptr);assert(marker_draws==59);
    mode=2;marker_draws=0;Map(raw,nullptr);assert(marker_draws==73);
    rectangles.clear();DrawPanel(raw,485,275,795,445,.5f);
    assert(!rectangles.empty()&&rectangles[0][0]==341&&rectangles[0][2]==122&&rectangles[0][4]==103);
    assert(color_state==colors&&memory==saved);
    ctx.r3.u64=4;ctx.r6.u64=0x44;ctx.r7.u64=0x55;ctx.r8.u64=0x66;ctx.f1.f64=.75;
    const auto mini_before=ctx;mode=3;marker_draws=0;MiniBatch(raw,nullptr);
    assert(native_minis==1&&extra_minis==1&&marker_draws==132&&memory==saved&&color_state==colors&&std::memcmp(&ctx,&mini_before,sizeof(ctx))==0);
    mode=0;MiniBatch(raw,nullptr);assert(extra_minis==1);
    mode=3;ctx.r3.u64=3;MiniBatch(raw,nullptr);assert(extra_minis==1);ctx.r3.u64=4;
    fail_draw=true;MiniBatch(raw,nullptr);assert(!enabled&&memory==saved&&color_state==colors);
    enabled=true;fail_draw=false;
    mode=3;fail_draw=true;Map(raw,nullptr);assert(!enabled&&U32(kHead)==0x90002000&&memory==saved&&color_state==colors);
    enabled=true;fail_draw=false;W8(0x8370E9F6,1);auto extra=extra_batches;Map(raw,nullptr);assert(extra_batches==extra);
    MiniBatch(raw,nullptr);assert(extra_batches==extra);
    memory[0xA0000000].resize(0x9000);
    std::ifstream asset(std::filesystem::path(__FILE__).parent_path()/"assets"/"ui-atlas.bin",std::ios::binary);
    atlas_data.assign(std::istreambuf_iterator<char>(asset),{});assert(atlas_data.size()==32768);
    {Scratch s(raw);EnsureAssets(s);}assert(assets_ready&&U32(0x8370DD60)==2&&U32(0x8370DD5C)==1);
    assert(U32(0x82FFAEC8+4)==1737&&U32(0x82FF5EC8+12)==38&&U32(0x82FF5EC8+20+12)==38);
    assert(!std::memcmp(Pointer(0xA0001000),atlas_data.data(),atlas_data.size()));
    WF(0x827AF11C,1);
    tracker::Location nearby{tracker::Kind::CD,0,{30,0,0},"test"};assert(InsideMinimap(nearby));
    nearby.pos[0]=47;assert(!InsideMinimap(nearby));nearby.pos[0]=130;assert(!InsideMinimap(nearby));
    custom_draw=true;marker_kind=tracker::Kind::CD;Bitmap(raw,nullptr);assert(drawn_bitmap==asset_bitmaps[0]);
    marker_kind=tracker::Kind::Graffiti;Bitmap(raw,nullptr);assert(drawn_bitmap==asset_bitmaps[1]);
    custom_draw=false;RS(raw,3,148);Bitmap(raw,nullptr);assert(drawn_bitmap==148);
    selector.focused=selector.capture_click=true;map_input_scope=true;
    for(auto action:{43,44,55}){RS(raw,3,action);MapAction(raw,nullptr);assert(R(raw,3)==0);}
    for(auto action:{0,37,38,56,57,58}){RS(raw,3,action);MapAction(raw,nullptr);assert(R(raw,3)==1);}
    map_input_scope=false;
    for(auto action:{43,44,55,56}){RS(raw,3,action);MapAction(raw,nullptr);assert(R(raw,3)==1);}
    selector={};
    std::cout<<"PASS: native CRC IDs, synthetic CTS parsing, all 135 installed locations, per-save completion filtering, four marker modes, native-font panel, full map and minimap passes, native minimap culling flags, map head/CPU context/stack/color restoration, failure restoration and multiplayer exclusion\n";
}
