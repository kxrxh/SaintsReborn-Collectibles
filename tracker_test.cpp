#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace {
bool input_focus=true;POINT input_cursor{640,360};
HWND TestForeground(){return input_focus?reinterpret_cast<HWND>(1):nullptr;}
DWORD TestWindowProcess(HWND,DWORD* process){*process=GetCurrentProcessId();return 1;}
BOOL TestClientRect(HWND,RECT* rect){*rect={0,0,1280,720};return TRUE;}
BOOL TestCursorPos(POINT* point){*point=input_cursor;return TRUE;}
BOOL TestScreenToClient(HWND,POINT*){return TRUE;}
}
#define GetForegroundWindow TestForeground
#define GetWindowThreadProcessId TestWindowProcess
#define GetClientRect TestClientRect
#define GetCursorPos TestCursorPos
#define ScreenToClient TestScreenToClient
#include "tracker.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetClientRect
#undef GetCursorPos
#undef ScreenToClient
#include <cassert>
#include <iostream>
#include <map>
#include <limits>
namespace {
std::map<uint32_t,std::vector<uint8_t>> memory;
WmlApi mock{};int native_batches=0,extra_batches=0,marker_draws=0,pos_fallbacks=0,native_minis=0,extra_minis=0;bool fail_draw=false,mini_pass=false;
std::array<unsigned,4> color_state{11,22,33,144};std::string overlay;
std::vector<std::string> labels;
uint32_t drawn_bitmap=0;
std::vector<std::array<int,5>> rectangles;
std::array<uint8_t,16> input_packet{0x12,0x34,0x56,0x78,0,0,0,0,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
std::array<bool,256> keys{};
std::map<unsigned,uint64_t> actions;unsigned pans=0;bool fail_update=false;
constexpr uint16_t kAccept=0x1000;
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
void NativeInput(WmlContext* c,uint8_t*){
    std::memcpy(Pointer(uint32_t(R(c,5))),input_packet.data(),input_packet.size());RS(c,3,0);input_cursor={640,360};
}
void NativePan(WmlContext*,uint8_t*){++pans;}
void NativeUpdate(WmlContext* c,uint8_t* b){
    for(auto action:{43,44,55,56,57,58}){RS(c,3,action);MapAction(c,b);actions[action]=R(c,3);}
    MapPan(c,b);if(fail_update)throw std::runtime_error("Simulated update failure");
}
void Poll(WmlContext* c,uint16_t buttons=0,bool mouse=false){
    input_packet[4]=uint8_t(buttons>>8);input_packet[5]=uint8_t(buttons);keys[VK_LBUTTON]=mouse;
    RS(c,3,0);RS(c,5,0x90013000);map_seen=GetTickCount64();ObserveInput(c,nullptr);
    assert(R(c,3)==0&&!std::memcmp(Pointer(0x90013000),input_packet.data(),input_packet.size()));
}

struct PackFixture{
    std::filesystem::path path;std::vector<uint8_t> bytes;
    void Put(size_t at,uint32_t value){for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(24-8*i));}
    explicit PackFixture(unsigned flags=1,bool flushed=false){
        std::string text="#Navpoints\n";
        for(unsigned i=0;i<60;++i)text+="$Navpoint: \"nav"+std::to_string(i)+"\"\n$Pos: <1 2 3>\n";
        text+="#Items\n";
        for(unsigned i=0;i<60;++i)text+="$Item: \"cd"+std::to_string(i)+"\"\n$Item type: \"CD_collect\"\n$Start nav: \"nav"+std::to_string(i)+"\"\n";
        text+="#Tagging Spots\n";
        for(unsigned i=0;i<75;++i)text+="$Tagging Spot: \"tag"+std::to_string(i)+"\"\n$Pos: <4 5 6>\n+Ambient\n";
        std::vector<uint8_t> data(text.begin(),text.end());
        if(flags&1){
            std::vector<uint8_t> compressed(compressBound(uLong(data.size()))+6);z_stream stream{};
            assert(deflateInit(&stream,Z_DEFAULT_COMPRESSION)==Z_OK);
            stream.next_in=data.data();stream.avail_in=uInt(data.size());stream.next_out=compressed.data();stream.avail_out=uInt(compressed.size());
            assert(deflate(&stream,flushed?Z_SYNC_FLUSH:Z_FINISH)==(flushed?Z_OK:Z_STREAM_END));
            const auto size=stream.total_out;const auto result=deflateEnd(&stream);
            assert(result==(flushed?Z_DATA_ERROR:Z_OK)); // An intentionally unfinished stream is still freed.
            compressed.resize(size);data=std::move(compressed);
        }
        bytes.resize(0x1800+data.size());std::copy(data.begin(),data.end(),bytes.begin()+0x1800);
        Put(0,0x51890ACE);Put(4,3);Put(0x14C,flags);Put(0x154,1);Put(0x15C,28);Put(0x160,15);Put(0x164,uint32_t(text.size()));Put(0x168,uint32_t(data.size()));
        Put(0x800+16,uint32_t(text.size()));Put(0x800+20,uint32_t(data.size()));
        constexpr char name[]="lc_tagging.cts";std::memcpy(bytes.data()+0x1000,name,sizeof(name));
        wchar_t temp[MAX_PATH],file[MAX_PATH];assert(GetTempPathW(MAX_PATH,temp));assert(GetTempFileNameW(temp,L"sct",0,file));path=file;
    }
    ~PackFixture(){std::error_code ignored;std::filesystem::remove(path,ignored);}
    std::vector<tracker::Location> Load(){
        {std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());assert(out.good());}
        return tracker::Pack(path).Locations();
    }
};
void Reject(PackFixture& fixture,const char* message){
    bool rejected=false;try{fixture.Load();}catch(const std::runtime_error& e){rejected=true;assert(std::string(e.what())==message);}assert(rejected);
}
void TestPackFiles(){
    for(unsigned flags=0;flags<4;++flags){PackFixture valid(flags);assert(valid.Load().size()==135);}
    for(unsigned flags:{1,3}){PackFixture valid(flags,true);assert(valid.Load().size()==135);}
    PackFixture bad_checksum;bad_checksum.bytes.back()^=1;Reject(bad_checksum,"Cannot decompress packfile entry");
    PackFixture truncated;truncated.bytes.pop_back();truncated.Put(0x800+20,uint32_t(truncated.bytes.size()-0x1800));Reject(truncated,"Cannot decompress packfile entry");
    PackFixture oversized;oversized.Put(0x800+20,64*1024*1024);Reject(oversized,"Invalid packfile range");
    PackFixture directory;directory.Put(0x15C,0xFFFFF800);Reject(directory,"Invalid packfile range");
    PackFixture offset(0);offset.Put(0x800+8,0xFFFFF000);Reject(offset,"Invalid packfile range");
    PackFixture condensed(3);condensed.Put(0x168,0xFFFFFFFF);Reject(condensed,"Invalid packfile range");
    PackFixture flush_marker(1,true);flush_marker.bytes.pop_back();flush_marker.Put(0x800+20,uint32_t(flush_marker.bytes.size()-0x1800));Reject(flush_marker,"Cannot decompress packfile entry");
    PackFixture corrupt_stream(1,true);corrupt_stream.bytes[0x1800]^=1;Reject(corrupt_stream,"Cannot decompress packfile entry");
}
void TestSelectorInput(WmlContext* raw){
    original_input=NativeInput;original_update=NativeUpdate;original_pan=NativePan;
    mock.key_down=[](int vk){return keys.at(vk)?1:0;};
    enabled=true;mode=3;selector={};W8(0x8370E9F6,0);W32(0x8370DDAC,26);
    const auto background=U32(0x827B05E8);W32(0x827B05E8,4096);
    const auto show=[&]{DrawPanel(raw,330,190,950,530,1);assert(selector.ready);};
    const auto update=[&](uint64_t filters,uint64_t waypoint,bool pan){
        const auto before=pans;MapUpdate(raw,nullptr);assert(!map_input_scope);
        assert(actions[43]==filters&&actions[44]==filters&&actions[55]==waypoint);
        for(auto action:{56,57,58})assert(actions[action]==1);
        assert(pans==before+unsigned(pan));
    };
    show();Poll(raw,kRight);assert(mode==3);update(1,1,true);Poll(raw);
    Poll(raw,kUp);assert(selector.focused);update(0,1,true);Poll(raw);
    Poll(raw,kRight);assert(mode==1);Poll(raw,kRight);assert(mode==1); // held button cycles once
    Poll(raw);Poll(raw,kRight);assert(mode==2);
    Poll(raw);Poll(raw,kRight);assert(mode==0);
    Poll(raw);Poll(raw,kLeft);assert(mode==2);
    Poll(raw);Poll(raw,kDown);assert(!selector.focused);update(1,1,true);Poll(raw);

    input_cursor={int((selector.left+selector.right)*.5f*1280),int((selector.top+selector.bottom)*.5f*720)};
    Poll(raw);assert(selector.pointer&&selector.focused);
    Poll(raw,kAccept,true);assert(mode==0&&selector.capture_click);update(0,0,false);
    keys['W']=true;update(0,0,true);keys['W']=false;
    input_cursor={1100,360};Poll(raw,kAccept,true);assert(mode==0&&selector.capture_click&&!selector.focused);update(1,0,true);
    Poll(raw);Poll(raw,kAccept,true);assert(mode==0&&!selector.capture_click);update(1,1,true);Poll(raw);
    for(const auto navigation:{0x10,0x20,0x100,0x200,0x2000,0x8000}){
        selector.focused=selector.pointer=selector.capture_click=true;
        Poll(raw,uint16_t(navigation));assert(mode==0&&!selector.focused&&!selector.capture_click);update(1,1,true);
    }
    for(auto menu:{46,0}){ // Co-op and leaving pause never inherit selector input.
        show();Poll(raw);Poll(raw,kUp);W32(0x8370DDAC,menu);Map(raw,nullptr);
        assert(!selector.ready&&!selector.focused);Poll(raw,kRight);assert(mode==0);update(1,1,true);
        W32(0x8370DDAC,26);
    }
    show();Poll(raw);Poll(raw,kUp);
    W32(0x90012000,960);labels.clear();rectangles.clear();DrawPanel(raw,170,190,790,530,1);
    assert(labels.empty()&&rectangles.empty()&&!selector.ready&&!selector.focused&&!selector.capture_click);
    Poll(raw,kRight);assert(mode==0);update(1,1,true);
    W32(0x90012000,1280);show();assert(!selector.focused);
    Poll(raw);Poll(raw,kUp);DrawPanel(raw,330,190,950,530,0);
    assert(!selector.ready&&!selector.focused);update(1,1,true);
    show();Poll(raw);Poll(raw,kUp);DrawPanel(raw,485,275,795,445,.5f);
    assert(!selector.ready&&!selector.focused);update(1,1,true);
    selector.focused=selector.pointer=selector.capture_click=true;update(1,1,true); // readiness also gates native actions
    HideSelector();show();Poll(raw);Poll(raw,kUp);
    input_focus=false;Poll(raw,kRight);assert(mode==0&&!selector.focused);update(1,1,true);input_focus=true;
    Poll(raw);Poll(raw,kUp);input_packet[6]=128;Poll(raw,kRight);assert(mode==0&&!selector.focused);update(1,1,true);input_packet[6]=0;
    fail_update=true;bool failed=false;try{MapUpdate(raw,nullptr);}catch(const std::runtime_error&){failed=true;}
    assert(failed&&!map_input_scope);fail_update=false;
    map_seen=GetTickCount64()-500;Frame(nullptr);assert(!selector.ready&&!selector.focused);
    W32(0x827B05E8,background);selector={};keys={};mode=3;
}
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
int main(int argc,char** argv)try{
    assert(argc==2);auto game=std::filesystem::path(argv[1]);
    TestPackFiles();
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
    mock.call=Call;mock.overlay_text=Overlay;api=&mock;static WmlMod mod{"Collectibles","test"};self=&mod;
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
    for(const float hud:{.75f,1.f,2.f}){
        WF(0x82FFB4E8,hud);WF(0x82FFB4EC,hud);
        for(unsigned angle=0;angle<360;angle+=15){
            const float radians=angle*3.14159265f/180;
            nearby.kind=tracker::Kind::Graffiti;nearby.pos={45.9f*hud*std::cos(radians),0,45.9f*hud*std::sin(radians)};assert(!InsideMinimap(nearby));
            nearby.pos={37.f*hud*std::cos(radians),0,37.f*hud*std::sin(radians)};assert(InsideMinimap(nearby));
            assert(std::hypot(nearby.pos[0],nearby.pos[2])+std::hypot(19.f,19.f)*hud+2*hud<=67*hud);
        }
    }
    WF(0x82FFB4E8,1);WF(0x82FFB4EC,1);
    custom_draw=true;marker_kind=tracker::Kind::CD;Bitmap(raw,nullptr);assert(drawn_bitmap==asset_bitmaps[0]);
    marker_kind=tracker::Kind::Graffiti;Bitmap(raw,nullptr);assert(drawn_bitmap==asset_bitmaps[1]);
    custom_draw=false;RS(raw,3,148);Bitmap(raw,nullptr);assert(drawn_bitmap==148);
    TestSelectorInput(raw);
    std::cout<<"PASS: valid/corrupt packfiles, native CRC IDs, CTS parsing, all 135 installed locations, completion filtering, map/minimap rendering and state restoration, diagonal radar bounds at multiple HUD scales, connected input hooks, hidden-panel input release, tab/close pass-through and multiplayer exclusion\n";
}catch(const std::exception& e){
    std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;
}
